#pragma once

#include "Memory.hpp"

namespace Coral {

	template<typename TValue>
	class Array
	{
	public:
		static Array New(size_t InLength)
		{
			Array<TValue> result;
			if (InLength > 0)
			{
				result.m_Ptr = static_cast<TValue*>(Memory::AllocHGlobal(InLength * sizeof(TValue)));
				result.m_Length = static_cast<int32_t>(InLength);
			}
			return result;
		}

		static Array New(const std::vector<TValue>& InValues)
		{
			Array<TValue> result;

			if (!InValues.empty())
			{
				result.m_Ptr = static_cast<TValue*>(Memory::AllocHGlobal(InValues.size() * sizeof(TValue)));
				result.m_Length = static_cast<int32_t>(InValues.size());
				memcpy(result.m_Ptr, InValues.data(), InValues.size() * sizeof(TValue));
			}

			return result;
		}

		static Array New(const void* buffer, size_t size)
		{
			Array<TValue> result;

			if (buffer)
			{
				result.m_Ptr = static_cast<TValue*>(Memory::AllocHGlobal(size));
				result.m_Length = static_cast<int32_t>(size);
				memcpy(result.m_Ptr, buffer, size);
			}

			return result;
		}

		static Array New(std::initializer_list<TValue> InValues)
		{
			Array result;
			
			if (InValues.size() > 0)
			{
				result.m_Ptr = static_cast<TValue*>(Memory::AllocHGlobal(InValues.size() * sizeof(TValue)));
				result.m_Length = static_cast<int32_t>(InValues.size());
				memcpy(result.m_Ptr, InValues.begin(), InValues.size() * sizeof(TValue));
			}

			return result;
		}

		static void Free(Array InArray)
		{
			if (!InArray.m_Ptr || InArray.m_Length == 0)
				return;

			Memory::FreeHGlobal(InArray.m_Ptr);
			InArray.m_Ptr = nullptr;
			InArray.m_Length = 0;
		}

		void Assign(const Array& InOther)
		{
			memcpy(m_Ptr, InOther.m_Ptr, InOther.m_Length * sizeof(TValue));
		}

		bool IsEmpty() const { return m_Length == 0 || m_Ptr == nullptr; }

		TValue& operator[](size_t InIndex) { return m_Ptr[InIndex]; }
		const TValue& operator[](size_t InIndex) const { return m_Ptr[InIndex]; }

		size_t Length() const { return m_Length; }
		size_t ByteLength() const { return m_Length * sizeof(TValue); }

		TValue* Data() { return m_Ptr; }
		const TValue* Data() const { return m_Ptr; }

		TValue* begin() { return m_Ptr; }
		TValue* end() { return m_Ptr + m_Length; }

		const TValue* begin() const { return m_Ptr; }
		const TValue* end() const { return m_Ptr + m_Length; }

		const TValue* cbegin() const { return m_Ptr; }
		const TValue* cend() const { return m_Ptr + m_Length; }

	private:
		TValue* m_Ptr = nullptr;
		TValue* m_ArrayHandle = nullptr;
		int32_t m_Length = 0;
		Bool32 m_IsDisposed = false;
	};

}
