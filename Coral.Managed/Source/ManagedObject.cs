using Coral.Managed.Interop;

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Coral.Managed;

using static ManagedHost;

internal enum ManagedType
{
	Unknown,

	SByte,
	Byte,
	Short,
	UShort,
	Int,
	UInt,
	Long,
	ULong,

	Float,
	Double,

	Bool,

	String,

	Pointer
};

internal static class ManagedObject
{

	public readonly struct MethodKey : IEquatable<MethodKey>
	{
		public readonly string TypeName;
		public readonly string Name;
		public readonly ManagedType[] Types;
		public readonly int ParameterCount;

		public MethodKey(string InTypeName, string InName, ManagedType[] InTypes, int InParameterCount)
		{
			TypeName = InTypeName;
			Name = InName;
			Types = InTypes;
			ParameterCount = InParameterCount;
		}

		public override bool Equals([NotNullWhen(true)] object? obj) => obj is MethodKey other && Equals(other);

		bool IEquatable<MethodKey>.Equals(MethodKey other)
		{
			if (TypeName != other.TypeName || Name != other.Name)
				return false;

			for (int i = 0; i < Types.Length; i++)
			{
				if (Types[i] != other.Types[i])
					return false;
			}

			return ParameterCount == other.ParameterCount;
		}

		public override int GetHashCode()
		{
			// NOTE(Peter): Josh Bloch's Hash (from https://stackoverflow.com/questions/263400/what-is-the-best-algorithm-for-overriding-gethashcode)
			unchecked
			{
				int hash = 17;

				hash = hash * 23 + TypeName.GetHashCode();
				hash = hash * 23 + Name.GetHashCode();
				foreach (var type in Types)
					hash = hash * 23 + type.GetHashCode();
				hash = hash * 23 + ParameterCount.GetHashCode();

				return hash;
			}
		}
	}

	internal static Dictionary<MethodKey, MethodInfo> s_CachedMethods = new Dictionary<MethodKey, MethodInfo>();

	[UnmanagedCallersOnly]
	internal static unsafe IntPtr CreateObject(int InTypeID, Bool32 InWeakRef, IntPtr InParameters, ManagedType* InParameterTypes, int InParameterCount)
	{
		try
		{
			if (!TypeInterface.s_CachedTypes.TryGetValue(InTypeID, out var type))
			{
				LogMessage($"Failed to find type with id '{InTypeID}'.", MessageLevel.Error);
				return IntPtr.Zero;
			}

			ConstructorInfo? constructor = null;

			var currentType = type;
			while (currentType != null)
			{
				ReadOnlySpan<ConstructorInfo> constructors = currentType.GetConstructors(BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Instance);
				
				constructor = TypeInterface.FindSuitableMethod(".ctor", InParameterTypes, InParameterCount, constructors);

				if (constructor != null)
					break;

				currentType = currentType.BaseType;
			}

			if (constructor == null)
			{
				LogMessage($"Failed to find constructor for type {type.FullName} with {InParameterCount} parameters.", MessageLevel.Error);
				return IntPtr.Zero;
			}

			var parameters = Marshalling.MarshalParameterArray(InParameters, InParameterCount, constructor);

			object? result = null;

			if (currentType != type || parameters == null)
			{
				result = TypeInterface.CreateInstance(type);

				if (currentType != type)
					constructor.Invoke(result, parameters);
			}
			else
			{
				result = TypeInterface.CreateInstance(type, parameters);
			}

			if (result == null)
			{
				LogMessage($"Failed to instantiate type {type.FullName}.", MessageLevel.Error);
			}

			var handle = GCHandle.Alloc(result, InWeakRef ? GCHandleType.Weak : GCHandleType.Normal);
#if DEBUG
			AssemblyLoader.RegisterHandle(type.Assembly, handle);
#endif
			return GCHandle.ToIntPtr(handle);
		}
		catch (Exception ex)
		{
			HandleException(ex);
			return IntPtr.Zero;
		}
	}

	[UnmanagedCallersOnly]
	internal static unsafe IntPtr CopyObject(IntPtr InObjectHandle)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InObjectHandle).Target;

			if (target == null)
			{
				LogMessage($"Cannot copy object with handle {InObjectHandle}. Target was null.", MessageLevel.Error);
				return IntPtr.Zero;
			}

			var handle = GCHandle.Alloc(target, GCHandleType.Normal);
#if DEBUG
			var type = target.GetType();
			AssemblyLoader.RegisterHandle(type.Assembly, handle);
#endif
			return GCHandle.ToIntPtr(handle);
		}
		catch (Exception ex)
		{
			HandleException(ex);
			return IntPtr.Zero;
		}
	}

	[UnmanagedCallersOnly]
	internal static void DestroyObject(IntPtr InObjectHandle)
	{
		try
		{
			GCHandle handle = GCHandle.FromIntPtr(InObjectHandle);
#if DEBUG
			var type = handle.Target?.GetType();
			if (type is not null) {
				AssemblyLoader.DeregisterHandle(type.Assembly, handle);
			}
#endif
			handle.Free();
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	private static unsafe MethodInfo? TryGetMethodInfo(Type InType, string InMethodName, ManagedType* InParameterTypes, int InParameterCount, BindingFlags InBindingFlags)
	{
		MethodInfo? methodInfo = null;

		var parameterTypes = new ManagedType[InParameterCount];

		unsafe
		{
			fixed (ManagedType* parameterTypesPtr = parameterTypes)
			{
				ulong size = sizeof(ManagedType) * (ulong)InParameterCount;
				Buffer.MemoryCopy(InParameterTypes, parameterTypesPtr, size, size);
			}
		}

		var methodKey = new MethodKey(InType.FullName, InMethodName, parameterTypes, InParameterCount);

		if (!s_CachedMethods.TryGetValue(methodKey, out methodInfo))
		{
			List<MethodInfo> methods = new(InType.GetMethods(InBindingFlags));

			Type? baseType = InType.BaseType;
			while (baseType != null)
			{
				methods.AddRange(baseType.GetMethods(InBindingFlags));
				baseType = baseType.BaseType;
			}

			methodInfo = TypeInterface.FindSuitableMethod<MethodInfo>(InMethodName, InParameterTypes, InParameterCount, CollectionsMarshal.AsSpan(methods));

			if (methodInfo == null)
			{
				LogMessage($"Failed to find method '{InMethodName}' for type {InType.FullName} with {InParameterCount} parameters.", MessageLevel.Error);
				return null;
			}

			s_CachedMethods.Add(methodKey, methodInfo);
		}

		return methodInfo;
	}

	[UnmanagedCallersOnly]
	internal static unsafe void InvokeStaticMethod(int InType, NativeString InMethodName, IntPtr InParameters, ManagedType* InParameterTypes, int InParameterCount)
	{
		try
		{
			if (!TypeInterface.s_CachedTypes.TryGetValue(InType, out var type))
			{
				LogMessage($"Cannot invoke method {InMethodName} on a null type.", MessageLevel.Error);
				return;
			}

			var methodInfo = TryGetMethodInfo(type, InMethodName, InParameterTypes, InParameterCount, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
			var parameters = Marshalling.MarshalParameterArray(InParameters, InParameterCount, methodInfo);

			methodInfo.Invoke(null, parameters);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static unsafe void InvokeStaticMethodRet(int InType, NativeString InMethodName, IntPtr InParameters, ManagedType* InParameterTypes, int InParameterCount, IntPtr InResultStorage)
	{
		try
		{
			if (!TypeInterface.s_CachedTypes.TryGetValue(InType, out var type))
			{
				LogMessage($"Cannot invoke method {InMethodName} on a null type.", MessageLevel.Error);
				return;
			}

			var methodInfo = TryGetMethodInfo(type, InMethodName, InParameterTypes, InParameterCount, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
			var methodParameters = Marshalling.MarshalParameterArray(InParameters, InParameterCount, methodInfo);

			object? value = methodInfo.Invoke(null, methodParameters);

			if (value == null)
				return;

			Marshalling.MarshalReturnValue(null, value, methodInfo, InResultStorage);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static unsafe void InvokeMethod(IntPtr InObjectHandle, NativeString InMethodName, IntPtr InParameters, ManagedType* InParameterTypes, int InParameterCount)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InObjectHandle).Target;

			if (target == null)
			{
				LogMessage($"Cannot invoke method {InMethodName} on a null type.", MessageLevel.Error);
				return;
			}

			var targetType = target.GetType();

			var methodInfo = TryGetMethodInfo(targetType, InMethodName, InParameterTypes, InParameterCount, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
			var parameters = Marshalling.MarshalParameterArray(InParameters, InParameterCount, methodInfo);

			methodInfo.Invoke(target, parameters);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}
	
	[UnmanagedCallersOnly]
	internal static unsafe void InvokeMethodRet(IntPtr InObjectHandle, NativeString InMethodName, IntPtr InParameters, ManagedType* InParameterTypes, int InParameterCount, IntPtr InResultStorage)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InObjectHandle).Target;

			if (target == null)
			{
				LogMessage($"Cannot invoke method {InMethodName} on object with handle {InObjectHandle}. Target was null.", MessageLevel.Error);
				return;
			}

			var targetType = target.GetType();

			var methodInfo = TryGetMethodInfo(targetType, InMethodName, InParameterTypes, InParameterCount, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
			var methodParameters = Marshalling.MarshalParameterArray(InParameters, InParameterCount, methodInfo);
			
			object? value = methodInfo.Invoke(target, methodParameters);

			if (value == null)
				return;

			Marshalling.MarshalReturnValue(target, value, methodInfo, InResultStorage);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static void SetFieldValue(IntPtr InTarget, NativeString InFieldName, IntPtr InValue)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InTarget).Target;

			if (target == null)
			{
				LogMessage($"Cannot set value of field {InFieldName} on object with handle {InTarget}. Target was null.", MessageLevel.Error);
				return;
			}

			var targetType = target.GetType();
			var fieldInfo = targetType.GetField(InFieldName!, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);

			if (fieldInfo == null)
			{
				LogMessage($"Failed to find field '{InFieldName}' in type '{targetType.FullName}'.", MessageLevel.Error);
				return;
			}

			if (fieldInfo.FieldType == typeof(string))
			{
				NativeString value = (NativeString) Marshalling.MarshalPointer(InValue, typeof(NativeString));
				fieldInfo.SetValue(target, value.ToString());
			}
			else if (fieldInfo.FieldType == typeof(bool))
			{
				Bool32 value = (Bool32) Marshalling.MarshalPointer(InValue, typeof(Bool32));
				fieldInfo.SetValue(target, (bool) value);
			}
			else
			{
				object? value = Marshalling.MarshalPointer(InValue, fieldInfo.FieldType);
				fieldInfo.SetValue(target, value);
			}
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static void GetFieldValue(IntPtr InTarget, NativeString InFieldName, IntPtr OutValue)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InTarget).Target;

			if (target == null)
			{
				LogMessage($"Cannot get value of field {InFieldName} from object with handle {InTarget}. Target was null.", MessageLevel.Error);
				return;
			}

			var targetType = target.GetType();
			var fieldInfo = targetType.GetField(InFieldName!, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);

			if (fieldInfo == null)
			{
				LogMessage($"Failed to find field '{InFieldName}' in type '{targetType.FullName}'.", MessageLevel.Error);
				return;
			}

			// Handles strings gracefully internally.
			Marshalling.MarshalReturnValue(target, fieldInfo.GetValue(target), fieldInfo, OutValue);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static void SetPropertyValue(IntPtr InTarget, NativeString InPropertyName, IntPtr InValue)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InTarget).Target;

			if (target == null)
			{
				LogMessage($"Cannot set value of property {InPropertyName} on object with handle {InTarget}. Target was null.", MessageLevel.Error);
				return;
			}

			var targetType = target.GetType();
			var propertyInfo = targetType.GetProperty(InPropertyName!, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
		
			if (propertyInfo == null)
			{
				LogMessage($"Failed to find property '{InPropertyName}' in type '{targetType.FullName}'", MessageLevel.Error);
				return;
			}

			if (propertyInfo.SetMethod == null)
			{
				LogMessage($"Cannot set value of property '{InPropertyName}'. No setter was found.", MessageLevel.Error);
				return;
			}

			object? value = Marshalling.MarshalPointer(InValue, propertyInfo.PropertyType);
			propertyInfo.SetValue(target, value);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static void GetPropertyValue(IntPtr InTarget, NativeString InPropertyName, IntPtr OutValue)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InTarget).Target;

			if (target == null)
			{
				LogMessage($"Cannot get value of property '{InPropertyName}' from object with handle {InTarget}. Target was null.", MessageLevel.Error);
				return;
			}

			var targetType = target.GetType();
			var propertyInfo = targetType.GetProperty(InPropertyName!, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);

			if (propertyInfo == null)
			{
				LogMessage($"Failed to find property '{InPropertyName}' in type '{targetType.FullName}'.", MessageLevel.Error);
				return;
			}

			if (propertyInfo.GetMethod == null)
			{
				LogMessage($"Cannot get value of property '{InPropertyName}'. No getter was found.", MessageLevel.Error);
				return;
			}

			Marshalling.MarshalReturnValue(target, propertyInfo.GetValue(target), propertyInfo, OutValue);
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}

	[UnmanagedCallersOnly]
	internal static unsafe void GetObjectTypeId(IntPtr InTarget, int* typeId)
	{
		try
		{
			var target = GCHandle.FromIntPtr(InTarget).Target;

			if (target == null)
			{
				LogMessage($"Cannot get type of object. Target was null.", MessageLevel.Error);
				return;
			}

			*typeId = TypeInterface.s_CachedTypes.Add(target.GetType());
		}
		catch (Exception ex)
		{
			HandleException(ex);
		}
	}
}
