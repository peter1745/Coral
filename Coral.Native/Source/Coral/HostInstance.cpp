#include "HostInstance.hpp"
#include "Verify.hpp"
#include "HostFXRErrorCodes.hpp"
#include "StringHelper.hpp"
#include "TypeCache.hpp"

#include "CoralManagedFunctions.hpp"

#if defined(CORAL_WINDOWS)
	#include <ShlObj_core.h>
#else
	#include <dlfcn.h>
#endif

namespace Coral {

	struct CoreCLRFunctions
	{
		hostfxr_set_error_writer_fn SetHostFXRErrorWriter = nullptr;
		hostfxr_set_runtime_property_value_fn SetRuntimePropertyValue = nullptr;
		hostfxr_initialize_for_runtime_config_fn InitHostFXRForRuntimeConfig = nullptr;
		hostfxr_get_runtime_delegate_fn GetRuntimeDelegate = nullptr;
		hostfxr_close_fn CloseHostFXR = nullptr;
		load_assembly_and_get_function_pointer_fn GetManagedFunctionPtr = nullptr;
	};
	static CoreCLRFunctions s_CoreCLRFunctions;

	MessageCallbackFn MessageCallback = nullptr;
	MessageLevel MessageFilter;
	ExceptionCallbackFn ExceptionCallback = nullptr;

	void DefaultMessageCallback(std::string_view InMessage, MessageLevel InLevel)
	{
		const char* level = "";

		switch (InLevel)
		{
		case MessageLevel::Info:
			level = "Info";
			break;
		case MessageLevel::Warning:
			level = "Warn";
			break;
		case MessageLevel::Error:
			level = "Error";
			break;
		}

		std::cout << "[Coral](" << level << "): " << InMessage << std::endl;
	}

	CoralInitStatus HostInstance::Initialize(HostSettings InSettings)
	{
		CORAL_VERIFY(!m_Initialized);

		if (!LoadHostFXR())
		{
			return CoralInitStatus::DotNetNotFound;
		}

		// Setup settings
		m_Settings = std::move(InSettings);

		if (!m_Settings.MessageCallback)
			m_Settings.MessageCallback = DefaultMessageCallback;
		MessageCallback = m_Settings.MessageCallback;
		MessageFilter = m_Settings.MessageFilter;

		s_CoreCLRFunctions.SetHostFXRErrorWriter([](const CharType* InMessage)
		{
			auto message = StringHelper::ConvertWideToUtf8(InMessage);
			MessageCallback(message, MessageLevel::Error);
		});

		m_CoralManagedAssemblyPath = std::filesystem::path(m_Settings.CoralDirectory) / "Coral.Managed.dll";

		if (!std::filesystem::exists(m_CoralManagedAssemblyPath))
		{
			MessageCallback("Failed to find Coral.Managed.dll", MessageLevel::Error);
			return CoralInitStatus::CoralManagedNotFound;
		}

		if (!InitializeCoralManaged())
		{
			return CoralInitStatus::CoralManagedInitError;
		}

		return CoralInitStatus::Success;
	}

	void HostInstance::Shutdown()
	{
		s_CoreCLRFunctions.CloseHostFXR(m_HostFXRContext);
	}
	
	AssemblyLoadContext HostInstance::CreateAssemblyLoadContext(std::string_view InName)
	{
		ScopedString name = String::New(InName);
		AssemblyLoadContext alc;
		alc.m_ContextId = s_ManagedFunctions.CreateAssemblyLoadContextFptr(name);
		alc.m_Host = this;
		return alc;
	}

	void HostInstance::UnloadAssemblyLoadContext(AssemblyLoadContext& InLoadContext)
	{
		s_ManagedFunctions.UnloadAssemblyLoadContextFptr(InLoadContext.m_ContextId);
		InLoadContext.m_ContextId = -1;
		InLoadContext.m_LoadedAssemblies.Clear();
	}

#ifdef _WIN32
	template <typename TFunc>
	TFunc LoadFunctionPtr(void* InLibraryHandle, const char* InFunctionName)
	{
		auto result = (TFunc)GetProcAddress((HMODULE)InLibraryHandle, InFunctionName);
		CORAL_VERIFY(result);
		return result;
	}
#else
	template <typename TFunc>
	TFunc LoadFunctionPtr(void* InLibraryHandle, const char* InFunctionName)
	{
		auto result = (TFunc)dlsym(InLibraryHandle, InFunctionName);
		CORAL_VERIFY(result);
		return result;
	}
#endif

	std::filesystem::path GetHostFXRPath()
	{
#if defined(CORAL_WINDOWS)
		std::filesystem::path basePath = "";
		
		// Find the Program Files folder
		TCHAR pf[MAX_PATH];
		SHGetSpecialFolderPath(
		nullptr,
		pf,
		CSIDL_PROGRAM_FILES,
		FALSE);

		basePath = pf;
		basePath /= "dotnet/host/fxr/";

		auto searchPaths = std::array
		{
			basePath
		};

#elif defined(CORAL_LINUX)
		auto searchPaths = std::array
		{
			std::filesystem::path("/usr/lib/dotnet/host/fxr/"),
			std::filesystem::path("/usr/share/dotnet/host/fxr/"),
		};
#elif defined(CORAL_MACOSX)
        auto searchPaths = std::array
        {
            std::filesystem::path("/usr/lib/dotnet/host/fxr/"),
            std::filesystem::path("/usr/local/share/dotnet/host/fxr/"),
        };
#endif

		for (const auto& path : searchPaths)
		{
			if (!std::filesystem::exists(path))
				continue;

			for (const auto& dir : std::filesystem::recursive_directory_iterator(path))
			{
				if (!dir.is_directory())
					continue;

				auto dirPath = dir.path().string();

				if (dirPath.find(CORAL_DOTNET_TARGET_VERSION_MAJOR_STR) == std::string::npos)
					continue;

				auto res = dir / std::filesystem::path(CORAL_HOSTFXR_NAME);
				CORAL_VERIFY(std::filesystem::exists(res));
				return res;
			}
		}

		return "";
	}

	bool HostInstance::LoadHostFXR() const
	{
		// Retrieve the file path to the CoreCLR library
		auto hostfxrPath = GetHostFXRPath();

		if (hostfxrPath.empty())
		{
			return false;
		}

		// Load the CoreCLR library
		void* libraryHandle = nullptr;

#ifdef CORAL_WINDOWS
	#ifdef CORAL_WIDE_CHARS
		libraryHandle = LoadLibraryW(hostfxrPath.c_str());
	#else
		libraryHandle = LoadLibraryA(hostfxrPath.string().c_str());
	#endif
#else
		libraryHandle = dlopen(hostfxrPath.string().data(), RTLD_NOW | RTLD_GLOBAL);
#endif

		if (libraryHandle == nullptr)
		{
			return false;
		}

		// Load CoreCLR functions
		s_CoreCLRFunctions.SetHostFXRErrorWriter = LoadFunctionPtr<hostfxr_set_error_writer_fn>(libraryHandle, "hostfxr_set_error_writer");
		s_CoreCLRFunctions.SetRuntimePropertyValue = LoadFunctionPtr<hostfxr_set_runtime_property_value_fn>(libraryHandle, "hostfxr_set_runtime_property_value");
		s_CoreCLRFunctions.InitHostFXRForRuntimeConfig = LoadFunctionPtr<hostfxr_initialize_for_runtime_config_fn>(libraryHandle, "hostfxr_initialize_for_runtime_config");
		s_CoreCLRFunctions.GetRuntimeDelegate = LoadFunctionPtr<hostfxr_get_runtime_delegate_fn>(libraryHandle, "hostfxr_get_runtime_delegate");
		s_CoreCLRFunctions.CloseHostFXR = LoadFunctionPtr<hostfxr_close_fn>(libraryHandle, "hostfxr_close");

		return true;
	}
	
	bool HostInstance::InitializeCoralManaged()
	{
		// Fetch load_assembly_and_get_function_pointer_fn from CoreCLR
		{
			auto runtimeConfigPath = std::filesystem::path(m_Settings.CoralDirectory) / "Coral.Managed.runtimeconfig.json";

			if (!std::filesystem::exists(runtimeConfigPath))
			{
				MessageCallback("Failed to find Coral.Managed.runtimeconfig.json", MessageLevel::Error);
				return false;
			}

			int status = s_CoreCLRFunctions.InitHostFXRForRuntimeConfig(runtimeConfigPath.c_str(), nullptr, &m_HostFXRContext);
			CORAL_VERIFY(status == StatusCode::Success || status == StatusCode::Success_HostAlreadyInitialized || status == StatusCode::Success_DifferentRuntimeProperties);
			CORAL_VERIFY(m_HostFXRContext != nullptr);

			std::filesystem::path coralDirectoryPath = m_Settings.CoralDirectory;
	#ifdef CORAL_WIDE_CHARS
			s_CoreCLRFunctions.SetRuntimePropertyValue(m_HostFXRContext, L"APP_CONTEXT_BASE_DIRECTORY", coralDirectoryPath.c_str());
	#else
			s_CoreCLRFunctions.SetRuntimePropertyValue(m_HostFXRContext, "APP_CONTEXT_BASE_DIRECTORY", coralDirectoryPath.c_str());
	#endif

			status = s_CoreCLRFunctions.GetRuntimeDelegate(m_HostFXRContext, hdt_load_assembly_and_get_function_pointer, (void**)&s_CoreCLRFunctions.GetManagedFunctionPtr);
			CORAL_VERIFY(status == StatusCode::Success);
		}

		using InitializeFn = void(*)(void(*)(String, MessageLevel), void(*)(String));
		InitializeFn coralManagedEntryPoint = nullptr;
		coralManagedEntryPoint = LoadCoralManagedFunctionPtr<InitializeFn>(CORAL_STR("Coral.Managed.ManagedHost, Coral.Managed"), CORAL_STR("Initialize"));

		LoadCoralFunctions();

		coralManagedEntryPoint([](String InMessage, MessageLevel InLevel)
		{
			if (MessageFilter & InLevel)
			{
				std::string message = InMessage;
				MessageCallback(message, InLevel);
			}
		},
		[](String InMessage)
		{
			std::string message = InMessage;
			if (!ExceptionCallback)
			{
				MessageCallback(message, MessageLevel::Error);
				return;
			}
			
			ExceptionCallback(message);
		});

		ExceptionCallback = m_Settings.ExceptionCallback;

		return true;
	}

	void HostInstance::LoadCoralFunctions()
	{
		s_ManagedFunctions.CreateAssemblyLoadContextFptr = LoadCoralManagedFunctionPtr<CreateAssemblyLoadContextFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("CreateAssemblyLoadContext"));
		s_ManagedFunctions.UnloadAssemblyLoadContextFptr = LoadCoralManagedFunctionPtr<UnloadAssemblyLoadContextFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("UnloadAssemblyLoadContext"));
		s_ManagedFunctions.LoadAssemblyFptr = LoadCoralManagedFunctionPtr<LoadAssemblyFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("LoadAssembly"));
		s_ManagedFunctions.LoadAssemblyFromMemoryFptr = LoadCoralManagedFunctionPtr<LoadAssemblyFromMemoryFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("LoadAssemblyFromMemory"));
		s_ManagedFunctions.UnloadAssemblyLoadContextFptr = LoadCoralManagedFunctionPtr<UnloadAssemblyLoadContextFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("UnloadAssemblyLoadContext"));
		s_ManagedFunctions.GetLastLoadStatusFptr = LoadCoralManagedFunctionPtr<GetLastLoadStatusFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("GetLastLoadStatus"));
		s_ManagedFunctions.GetAssemblyNameFptr = LoadCoralManagedFunctionPtr<GetAssemblyNameFn>(CORAL_STR("Coral.Managed.AssemblyLoader, Coral.Managed"), CORAL_STR("GetAssemblyName"));

		s_ManagedFunctions.GetAssemblyTypesFptr = LoadCoralManagedFunctionPtr<GetAssemblyTypesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAssemblyTypes"));
		s_ManagedFunctions.GetTypeIdFptr = LoadCoralManagedFunctionPtr<GetTypeIdFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeId"));
		s_ManagedFunctions.GetFullTypeNameFptr = LoadCoralManagedFunctionPtr<GetFullTypeNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFullTypeName"));
		s_ManagedFunctions.GetAssemblyQualifiedNameFptr = LoadCoralManagedFunctionPtr<GetAssemblyQualifiedNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAssemblyQualifiedName"));
		s_ManagedFunctions.GetBaseTypeFptr = LoadCoralManagedFunctionPtr<GetBaseTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetBaseType"));
		s_ManagedFunctions.GetTypeSizeFptr = LoadCoralManagedFunctionPtr<GetTypeSizeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeSize"));
		s_ManagedFunctions.IsTypeSubclassOfFptr = LoadCoralManagedFunctionPtr<IsTypeSubclassOfFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeSubclassOf"));
		s_ManagedFunctions.IsTypeAssignableToFptr = LoadCoralManagedFunctionPtr<IsTypeAssignableToFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeAssignableTo"));
		s_ManagedFunctions.IsTypeAssignableFromFptr = LoadCoralManagedFunctionPtr<IsTypeAssignableFromFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeAssignableFrom"));
		s_ManagedFunctions.IsTypeSZArrayFptr = LoadCoralManagedFunctionPtr<IsTypeSZArrayFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("IsTypeSZArray"));
		s_ManagedFunctions.GetElementTypeFptr = LoadCoralManagedFunctionPtr<GetElementTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetElementType"));
		s_ManagedFunctions.GetTypeMethodsFptr = LoadCoralManagedFunctionPtr<GetTypeMethodsFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeMethods"));
		s_ManagedFunctions.GetTypeFieldsFptr = LoadCoralManagedFunctionPtr<GetTypeFieldsFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeFields"));
		s_ManagedFunctions.GetTypePropertiesFptr = LoadCoralManagedFunctionPtr<GetTypePropertiesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeProperties"));
		s_ManagedFunctions.HasTypeAttributeFptr = LoadCoralManagedFunctionPtr<HasTypeAttributeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("HasTypeAttribute"));
		s_ManagedFunctions.GetTypeAttributesFptr = LoadCoralManagedFunctionPtr<GetTypeAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeAttributes"));
		s_ManagedFunctions.GetTypeManagedTypeFptr = LoadCoralManagedFunctionPtr<GetTypeManagedTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetTypeManagedType"));
		s_ManagedFunctions.InvokeStaticMethodFptr = LoadCoralManagedFunctionPtr<InvokeStaticMethodFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeStaticMethod"));
		s_ManagedFunctions.InvokeStaticMethodRetFptr = LoadCoralManagedFunctionPtr<InvokeStaticMethodRetFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeStaticMethodRet"));

		s_ManagedFunctions.GetMethodInfoNameFptr = LoadCoralManagedFunctionPtr<GetMethodInfoNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoName"));
		s_ManagedFunctions.GetMethodInfoReturnTypeFptr = LoadCoralManagedFunctionPtr<GetMethodInfoReturnTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoReturnType"));
		s_ManagedFunctions.GetMethodInfoParameterTypesFptr = LoadCoralManagedFunctionPtr<GetMethodInfoParameterTypesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoParameterTypes"));
		s_ManagedFunctions.GetMethodInfoAccessibilityFptr = LoadCoralManagedFunctionPtr<GetMethodInfoAccessibilityFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoAccessibility"));
		s_ManagedFunctions.GetMethodInfoAttributesFptr = LoadCoralManagedFunctionPtr<GetMethodInfoAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetMethodInfoAttributes"));

		s_ManagedFunctions.GetFieldInfoNameFptr = LoadCoralManagedFunctionPtr<GetFieldInfoNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoName"));
		s_ManagedFunctions.GetFieldInfoTypeFptr = LoadCoralManagedFunctionPtr<GetFieldInfoTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoType"));
		s_ManagedFunctions.GetFieldInfoAccessibilityFptr = LoadCoralManagedFunctionPtr<GetFieldInfoAccessibilityFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoAccessibility"));
		s_ManagedFunctions.GetFieldInfoAttributesFptr = LoadCoralManagedFunctionPtr<GetFieldInfoAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetFieldInfoAttributes"));

		s_ManagedFunctions.GetPropertyInfoNameFptr = LoadCoralManagedFunctionPtr<GetPropertyInfoNameFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetPropertyInfoName"));
		s_ManagedFunctions.GetPropertyInfoTypeFptr = LoadCoralManagedFunctionPtr<GetPropertyInfoTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetPropertyInfoType"));
		s_ManagedFunctions.GetPropertyInfoAttributesFptr = LoadCoralManagedFunctionPtr<GetPropertyInfoAttributesFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetPropertyInfoAttributes"));

		s_ManagedFunctions.GetAttributeFieldValueFptr = LoadCoralManagedFunctionPtr<GetAttributeFieldValueFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAttributeFieldValue"));
		s_ManagedFunctions.GetAttributeTypeFptr = LoadCoralManagedFunctionPtr<GetAttributeTypeFn>(CORAL_STR("Coral.Managed.TypeInterface, Coral.Managed"), CORAL_STR("GetAttributeType"));

		s_ManagedFunctions.SetInternalCallsFptr = LoadCoralManagedFunctionPtr<SetInternalCallsFn>(CORAL_STR("Coral.Managed.Interop.InternalCallsManager, Coral.Managed"), CORAL_STR("SetInternalCalls"));
		s_ManagedFunctions.CreateObjectFptr = LoadCoralManagedFunctionPtr<CreateObjectFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("CreateObject"));
		s_ManagedFunctions.CopyObjectFptr = LoadCoralManagedFunctionPtr<CopyObjectFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("CopyObject"));
		s_ManagedFunctions.InvokeMethodFptr = LoadCoralManagedFunctionPtr<InvokeMethodFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeMethod"));
		s_ManagedFunctions.InvokeMethodRetFptr = LoadCoralManagedFunctionPtr<InvokeMethodRetFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("InvokeMethodRet"));
		s_ManagedFunctions.SetFieldValueFptr = LoadCoralManagedFunctionPtr<SetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("SetFieldValue"));
		s_ManagedFunctions.GetFieldValueFptr = LoadCoralManagedFunctionPtr<GetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("GetFieldValue"));
		s_ManagedFunctions.SetPropertyValueFptr = LoadCoralManagedFunctionPtr<SetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("SetPropertyValue"));
		s_ManagedFunctions.GetPropertyValueFptr = LoadCoralManagedFunctionPtr<GetFieldValueFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("GetPropertyValue"));
		s_ManagedFunctions.DestroyObjectFptr = LoadCoralManagedFunctionPtr<DestroyObjectFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("DestroyObject"));
		s_ManagedFunctions.GetObjectTypeIdFptr = LoadCoralManagedFunctionPtr<GetObjectTypeIdFn>(CORAL_STR("Coral.Managed.ManagedObject, Coral.Managed"), CORAL_STR("GetObjectTypeId"));

		s_ManagedFunctions.CollectGarbageFptr = LoadCoralManagedFunctionPtr<CollectGarbageFn>(CORAL_STR("Coral.Managed.GarbageCollector, Coral.Managed"), CORAL_STR("CollectGarbage"));
		s_ManagedFunctions.WaitForPendingFinalizersFptr = LoadCoralManagedFunctionPtr<WaitForPendingFinalizersFn>(CORAL_STR("Coral.Managed.GarbageCollector, Coral.Managed"), CORAL_STR("WaitForPendingFinalizers"));
	}

	void* HostInstance::LoadCoralManagedFunctionPtr(const std::filesystem::path& InAssemblyPath, const CharType* InTypeName, const CharType* InMethodName, const CharType* InDelegateType) const
	{
		void* funcPtr = nullptr;
		int status = s_CoreCLRFunctions.GetManagedFunctionPtr(InAssemblyPath.c_str(), InTypeName, InMethodName, InDelegateType, nullptr, &funcPtr);
		CORAL_VERIFY(status == StatusCode::Success && funcPtr != nullptr);
		return funcPtr;
	}
}
