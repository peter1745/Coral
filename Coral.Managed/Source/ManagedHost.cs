using Coral.Managed.Interop;

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace Coral.Managed;

internal enum MessageLevel { Trace = 1, Info = 2, Warning = 4, Error = 8 }

internal static class ManagedHost
{
	private static unsafe delegate*<NativeString, void> s_ExceptionCallback;

	private static unsafe delegate*<NativeString, MessageLevel, void> s_MessageCallback;

	[UnmanagedCallersOnly]
	private static unsafe void Initialize(delegate*<NativeString, MessageLevel, void> InMessageCallback, delegate*<NativeString, void> InExceptionCallback)
	{
		s_MessageCallback = InMessageCallback;
		s_ExceptionCallback = InExceptionCallback;
	}

	internal static void LogMessage(string InMessage, MessageLevel InLevel)
	{
		unsafe
		{
			using NativeString message = InMessage;
			s_MessageCallback(message, InLevel);
		}
	}

	internal static void HandleException(Exception InException)
	{
		unsafe
		{
			if (s_ExceptionCallback == null)
				return;

			using NativeString message = InException.ToString();
			s_ExceptionCallback(message);
		}
	}

}
