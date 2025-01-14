#include "Assembly.hpp"
#include "HostInstance.hpp"
#include "CoralManagedFunctions.hpp"
#include "Verify.hpp"
#include "StringHelper.hpp"
#include "TypeCache.hpp"

namespace Coral {

	void ManagedAssembly::AddInternalCall(std::string_view InClassName, std::string_view InVariableName, void* InFunctionPtr)
	{
		CORAL_VERIFY(InFunctionPtr != nullptr);

		std::string assemblyQualifiedName(InClassName);
		assemblyQualifiedName += "+";
		assemblyQualifiedName += InVariableName;
		assemblyQualifiedName += ", ";
		assemblyQualifiedName += m_Name;

		const auto& name = m_InternalCallNameStorage.emplace_back(StringHelper::ConvertUtf8ToWide(assemblyQualifiedName));

		InternalCall internalCall;
		internalCall.Name = name.c_str();
		internalCall.NativeFunctionPtr = InFunctionPtr;
		m_InternalCalls.emplace_back(internalCall);
	}

	void ManagedAssembly::UploadInternalCalls()
	{
		s_ManagedFunctions.SetInternalCallsFptr(m_OwnerContextId, m_InternalCalls.data(), static_cast<int32_t>(m_InternalCalls.size()));
	}

	Type& ManagedAssembly::GetType(std::string_view InClassName) const
	{
		static Type s_NullType;
		Type* type = TypeCache::Get().GetTypeByName(InClassName);
		return type != nullptr ? *type : s_NullType;
	}

	const std::vector<Type*>& ManagedAssembly::GetTypes() const
	{
		return m_Types;
	}

	// TODO(Emily): Massive de-dup needed between `LoadAssembly` and `LoadAssemblyFromMemory`.
	ManagedAssembly& AssemblyLoadContext::LoadAssembly(std::string_view InFilePath)
	{
		auto filepath = String::New(InFilePath);

		auto[idx, result] = m_LoadedAssemblies.EmplaceBack();
		result.m_Host = m_Host;
		result.m_AssemblyId = s_ManagedFunctions.LoadAssemblyFptr(m_ContextId, filepath);
		result.m_OwnerContextId = m_ContextId;
		result.m_LoadStatus = s_ManagedFunctions.GetLastLoadStatusFptr();

		if (result.m_LoadStatus == AssemblyLoadStatus::Success)
		{
			auto assemblyName = s_ManagedFunctions.GetAssemblyNameFptr(m_ContextId, result.m_AssemblyId);
			result.m_Name = assemblyName;
			String::Free(assemblyName);

			// TODO(Emily): Is it always desirable to preload every type from an assembly?
			int32_t typeCount = 0;
			s_ManagedFunctions.GetAssemblyTypesFptr(m_ContextId, result.m_AssemblyId, nullptr, &typeCount);

			std::vector<TypeId> typeIds(static_cast<size_t>(typeCount));
			s_ManagedFunctions.GetAssemblyTypesFptr(m_ContextId, result.m_AssemblyId, typeIds.data(), &typeCount);

			for (auto typeId : typeIds)
			{
				Type type;
				type.m_Id = typeId;
				result.m_Types.push_back(TypeCache::Get().CacheType(std::move(type)));
			}
		}

		String::Free(filepath);

		return result;
	}

	ManagedAssembly& AssemblyLoadContext::LoadAssemblyFromMemory(const std::byte* data, int64_t dataLength)
	{
		auto [idx, result] = m_LoadedAssemblies.EmplaceBack();
		result.m_Host = m_Host;
		result.m_AssemblyId = s_ManagedFunctions.LoadAssemblyFromMemoryFptr(m_ContextId, data, dataLength);
		result.m_LoadStatus = s_ManagedFunctions.GetLastLoadStatusFptr();

		if (result.m_LoadStatus == AssemblyLoadStatus::Success)
		{
			auto assemblyName = s_ManagedFunctions.GetAssemblyNameFptr(m_ContextId, result.m_AssemblyId);
			result.m_Name = assemblyName;
			String::Free(assemblyName);

			int32_t typeCount = 0;
			s_ManagedFunctions.GetAssemblyTypesFptr(m_ContextId, result.m_AssemblyId, nullptr, &typeCount);
			std::vector<TypeId> typeIds(static_cast<size_t>(typeCount));
			s_ManagedFunctions.GetAssemblyTypesFptr(m_ContextId, result.m_AssemblyId, typeIds.data(), &typeCount);

			for (auto typeId : typeIds)
			{
				Type type;
				type.m_Id = typeId;
				result.m_Types.push_back(TypeCache::Get().CacheType(std::move(type)));
			}
		}

		return result;
	}

}
