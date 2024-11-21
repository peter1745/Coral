#pragma once

#include <string_view>

#if defined(CORAL_WINDOWS)
	#define CORAL_CALLTYPE __cdecl
	#define CORAL_HOSTFXR_NAME "hostfxr.dll"

	#ifdef _WCHAR_T_DEFINED
		#define CORAL_STR(s) L##s
		#define CORAL_WIDE_CHARS

		using CharType = wchar_t;
		using StringView = std::wstring_view;

	#else
		#define CORAL_STR(s) s

		using CharType = unsigned short;
		using StringView = std::string_view;
	#endif
#elif defined(CORAL_LINUX)
	#define CORAL_CALLTYPE
	#define CORAL_STR(s) s
	#define CORAL_HOSTFXR_NAME "libhostfxr.so"

	using CharType = char;
	using StringView = std::string_view;

#else
    #define CORAL_CALLTYPE
    #define CORAL_STR(s) s
    #define CORAL_HOSTFXR_NAME "libhostfxr.dylib"

    using CharType = char;
    using StringView = std::string_view;

#endif

#define CORAL_DOTNET_TARGET_VERSION_MAJOR 8
#define CORAL_DOTNET_TARGET_VERSION_MAJOR_STR '8'
#define CORAL_UNMANAGED_CALLERS_ONLY ((const CharType*)-1)

namespace Coral {

	using Bool32 = uint32_t;

	enum class TypeAccessibility
	{
		Public,
		Private,
		Protected,
		Internal,
		ProtectedPublic,
		PrivateProtected
	};

	using TypeId = int32_t;
	using ManagedHandle = int32_t;

	struct InternalCall
	{
		const CharType* Name;
		void* NativeFunctionPtr;
	};

}
