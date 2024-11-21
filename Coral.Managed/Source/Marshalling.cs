using Coral.Managed.Interop;

using System;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Coral.Managed;

public static class Marshalling
{
	// This needs to map to Coral::Array, hence the unused ArrayHandle
	struct ValueArrayContainer
	{
		public IntPtr Data;
		public IntPtr ArrayHandle;
		public int Length;
	};

	// This needs to map to Coral::Array, hence the unused ArrayHandle
	struct ObjectArrayContainer
	{
		public IntPtr Data;
		public IntPtr ArrayHandle;
		public int Length;
	};

	public static void MarshalReturnValue(object? InTarget, object? InValue, MemberInfo? InMemberInfo, IntPtr OutValue)
	{
		if (InMemberInfo == null)
			return;

		Type? type = null;

		if (InMemberInfo is FieldInfo fieldInfo)
		{
			type = fieldInfo.FieldType;
		}
		else if (InMemberInfo is PropertyInfo propertyInfo)
		{
			type = propertyInfo.PropertyType;
		}
		else if (InMemberInfo is MethodInfo methodInfo)
		{
			type = methodInfo.ReturnType;
		}

		if (type.IsSZArray)
		{
			var fieldArray = ArrayStorage.GetFieldArray(InTarget, InValue, InMemberInfo);

			if (fieldArray != null)
			{
				Marshal.WriteIntPtr(OutValue, fieldArray.Value.AddrOfPinnedObject());
			}
			else
			{
				Marshal.WriteIntPtr(OutValue, IntPtr.Zero);
			}
		}
		else if (InValue is string)
		{
			NativeString nativeString = (NativeString) (string) InValue;
			Marshal.StructureToPtr(nativeString, OutValue, false);
		}
		else if (InValue is bool)
		{
			Bool32 value = (Bool32) (bool) InValue;
			Marshal.StructureToPtr(value, OutValue, false);
		}
		else if (InValue is NativeString)
		{
			NativeString nativeString = (NativeString) InValue;
			Marshal.StructureToPtr((NativeString) InValue, OutValue, false);
		}
		else if (type.IsPointer)
		{
			unsafe
			{
				if (InValue == null)
				{
					Marshal.WriteIntPtr(OutValue, IntPtr.Zero);
				}
				else
				{
					void* valuePointer = Pointer.Unbox(InValue);
					Buffer.MemoryCopy(&valuePointer, OutValue.ToPointer(), IntPtr.Size, IntPtr.Size);
				}
			}
		}
		else
		{
			int valueSize = type.IsEnum ? Marshal.SizeOf(Enum.GetUnderlyingType(type)) : Marshal.SizeOf(type);
			var handle = GCHandle.Alloc(InValue, GCHandleType.Pinned);

			unsafe
			{
				Buffer.MemoryCopy(handle.AddrOfPinnedObject().ToPointer(), OutValue.ToPointer(), valueSize, valueSize);
			}

			handle.Free();
		}
	}

	private struct ArrayObject
	{
		public IntPtr Handle;
		public IntPtr Padding;
	}

	public static object? MarshalArray(IntPtr InArray, Type? InElementType)
	{
		if (InElementType == null)
			return null;

		Array? result;

		if (InElementType.IsValueType)
		{
			var arrayContainer = MarshalPointer<ValueArrayContainer>(InArray);

			if (ArrayStorage.HasFieldArray(null, null))
			{
				var fieldArray = ArrayStorage.GetFieldArray(null, null, null);

				if (arrayContainer.Data == fieldArray!.Value.AddrOfPinnedObject())
				{
					return fieldArray.Value.Target;
				}
			}

			result = Array.CreateInstance(InElementType, arrayContainer.Length);

			int elementSize = Marshal.SizeOf(InElementType);

			unsafe
			{
				for (int i = 0; i < arrayContainer.Length; i++)
				{
					IntPtr source = (IntPtr)(((byte*)arrayContainer.Data.ToPointer()) + (i * elementSize));
					result.SetValue(Marshal.PtrToStructure(source, InElementType), i);
				}
			}
		}
		else
		{
			var arrayContainer = MarshalPointer<ObjectArrayContainer>(InArray);

			if (ArrayStorage.HasFieldArray(null, null))
			{
				var fieldArray = ArrayStorage.GetFieldArray(null, null, null);

				if (arrayContainer.Data == fieldArray!.Value.AddrOfPinnedObject())
				{
					return fieldArray.Value.Target;
				}
			}

			result = Array.CreateInstance(InElementType, arrayContainer.Length);
			
			unsafe
			{
				for (int i = 0; i < arrayContainer.Length; i++)
				{
					IntPtr source = (IntPtr)(((byte*)arrayContainer.Data.ToPointer()) + (i * Marshal.SizeOf<ArrayObject>()));
					var managedObject = MarshalPointer<ArrayObject>(source);
					var target = GCHandle.FromIntPtr(managedObject.Handle).Target;
					result.SetValue(target, i);
				}
			}
		}

		return result;
	}

	/*public static void CopyArrayToBuffer(GCHandle InArrayHandle, Array? InArray, Type? InElementType)
	{
		if (InArray == null || InElementType == null)
			return;

		var elementSize = Marshal.SizeOf(InElementType);
		int byteLength = InArray.Length * elementSize;

		int offset = 0;

		foreach (var elem in InArray)
		{
			var elementHandle = GCHandle.Alloc(elem, GCHandleType.Pinned);

			unsafe
			{
				Buffer.MemoryCopy(elementHandle.AddrOfPinnedObject().ToPointer(), ((byte*)mem.ToPointer()) + offset, elementSize, elementSize);
			}

			offset += elementSize;

			elementHandle.Free();
		}

		ArrayContainer container = new()
		{
			Data = mem,
			Length = InArray.Length
		};

		var handle = GCHandle.Alloc(container, GCHandleType.Pinned);
		
		unsafe
		{
			Buffer.MemoryCopy(handle.AddrOfPinnedObject().ToPointer(), InBuffer.ToPointer(), Marshal.SizeOf<ArrayContainer>(), Marshal.SizeOf<ArrayContainer>());
		}

		handle.Free();
	}*/

	public static object? MarshalPointer(IntPtr InValue, Type InType)
	{
		if (InType.IsPointer || InType == typeof(IntPtr))
			return InValue;

		// NOTE(Peter): Marshal.PtrToStructure<bool> doesn't seem to work
		//				instead we have to read it as a single byte and check the raw value
		if (InType == typeof(bool))
			return Marshal.PtrToStructure<byte>(InValue) > 0;

		if (InType == typeof(string))
		{
			var nativeString = Marshal.PtrToStructure<NativeString>(InValue);
			return nativeString.ToString();
		}
		else if (InType == typeof(NativeString))
		{
			return Marshal.PtrToStructure<NativeString>(InValue);
		}

		if (InType.IsSZArray)
			return MarshalArray(InValue, InType.GetElementType());

		if (InType.IsGenericType)
		{
			if (InType == typeof(NativeArray<>).MakeGenericType(InType.GetGenericArguments().First()))
			{
				var elements = Marshal.ReadIntPtr(InValue, 0);
				var elementCount = Marshal.ReadInt32(InValue, Marshal.SizeOf<IntPtr>());
				var genericType = typeof(NativeArray<>).MakeGenericType(InType.GetGenericArguments().First());
				return TypeInterface.CreateInstance(genericType, elements, elementCount);
			}
		}

		if (InType.IsClass)
		{
			var handlePtr = Marshal.ReadIntPtr(InValue);
			var handle = GCHandle.FromIntPtr(handlePtr);
			return handle.Target;
		}

		return Marshal.PtrToStructure(InValue, InType);	
	}
	public static T? MarshalPointer<T>(IntPtr InValue) => Marshal.PtrToStructure<T>(InValue);

	public static IntPtr[] NativeArrayToIntPtrArray(IntPtr InNativeArray, int InLength)
	{
		try
		{
			if (InNativeArray == IntPtr.Zero || InLength == 0)
				return [];

			IntPtr[] result = new IntPtr[InLength];

			for (int i = 0; i < InLength; i++)
				result[i] = Marshal.ReadIntPtr(InNativeArray, i * Marshal.SizeOf<nint>());

			return result;
		}
		catch (Exception ex)
		{
			ManagedHost.HandleException(ex);
			return [];
		}
	}

	public static object?[]? MarshalParameterArray(IntPtr InNativeArray, int InLength, MethodBase? InMethodInfo)
	{
		if (InMethodInfo == null)
			return null;

		if (InNativeArray == IntPtr.Zero || InLength == 0)
			return null;

		var parameterInfos = InMethodInfo.GetParameters();
		var parameterPointers = NativeArrayToIntPtrArray(InNativeArray, InLength);
		var result = new object?[parameterPointers.Length];

		for (int i = 0; i < parameterPointers.Length; i++)
		{
			result[i] = MarshalPointer(parameterPointers[i], parameterInfos[i].ParameterType);
		}

		return result;
	}
	
}
