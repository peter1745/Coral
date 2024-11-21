#pragma once

#include "Core.hpp"
#include "Utility.hpp"
#include "String.hpp"

namespace Coral {

	class ManagedAssembly;
	class Type;

	class ManagedObject
	{
	public:
		ManagedObject() = default;
		ManagedObject(const ManagedObject& InOther);
		ManagedObject(ManagedObject&& InOther) noexcept;
		~ManagedObject();

		ManagedObject& operator=(const ManagedObject& InOther);
		ManagedObject& operator=(ManagedObject&& InOther) noexcept;

		template<typename TReturn, typename... TArgs>
		TReturn InvokeMethod(std::string_view InMethodName, TArgs&&... InParameters) const
		{
			constexpr size_t parameterCount = sizeof...(InParameters);

			TReturn result;
			
			if constexpr (parameterCount > 0)
			{
				const void* parameterValues[parameterCount];
				ManagedType parameterTypes[parameterCount];
				AddToArray<TArgs...>(parameterValues, parameterTypes, std::forward<TArgs>(InParameters)..., std::make_index_sequence<parameterCount> {});
				InvokeMethodRetInternal(InMethodName, parameterValues, parameterTypes, parameterCount, &result);
			}
			else
			{
				InvokeMethodRetInternal(InMethodName, nullptr, nullptr, 0, &result);
			}

			return result;
		}

		template<typename... TArgs>
		void InvokeMethod(std::string_view InMethodName, TArgs&&... InParameters) const
		{
			constexpr size_t parameterCount = sizeof...(InParameters);

			if constexpr (parameterCount > 0)
			{
				const void* parameterValues[parameterCount];
				ManagedType parameterTypes[parameterCount];
				AddToArray<TArgs...>(parameterValues, parameterTypes, std::forward<TArgs>(InParameters)..., std::make_index_sequence<parameterCount> {});
				InvokeMethodInternal(InMethodName, parameterValues, parameterTypes, parameterCount);
			}
			else
			{
				InvokeMethodInternal(InMethodName, nullptr, nullptr, 0);
			}
		}

		template<typename TValue>
		void SetFieldValue(std::string_view InFieldName, TValue InValue) const
		{
			SetFieldValueRaw(InFieldName, &InValue);
		}

		template<>
		void SetFieldValue(std::string_view InFieldName, std::string InValue) const
		{
			String s = String::New(InValue);
			SetFieldValueRaw(InFieldName, &InValue);
			String::Free(s);
		}

		template<>
		void SetFieldValue(std::string_view InFieldName, bool InValue) const
		{
			Bool32 s = InValue;
			SetFieldValueRaw(InFieldName, &s);
		}

		template<typename TReturn>
		TReturn GetFieldValue(std::string_view InFieldName) const
		{
			TReturn result;
			GetFieldValueRaw(InFieldName, &result);
			return result;
		}

		template<>
		std::string GetFieldValue(std::string_view InFieldName) const
		{
			String result;
			GetFieldValueRaw(InFieldName, &result);
			auto s = std::string(result);
			String::Free(result);
			return s;
		}

		template<>
		bool GetFieldValue(std::string_view InFieldName) const
		{
			Bool32 result;
			GetFieldValueRaw(InFieldName, &result);
			return result;
		}

		template<typename TValue>
		void SetPropertyValue(std::string_view InPropertyName, TValue InValue) const
		{
			SetPropertyValueRaw(InPropertyName, &InValue);
		}

		template<typename TReturn>
		TReturn GetPropertyValue(std::string_view InPropertyName) const
		{
			TReturn result;
			GetPropertyValueRaw(InPropertyName, &result);
			return result;
		}

		void SetFieldValueRaw(std::string_view InFieldName, void* InValue) const;
		void GetFieldValueRaw(std::string_view InFieldName, void* OutValue) const;
		void SetPropertyValueRaw(std::string_view InPropertyName, void* InValue) const;
		void GetPropertyValueRaw(std::string_view InPropertyName, void* OutValue) const;

		const Type& GetType();
		
		void Destroy();

		bool IsValid() const { return m_Handle != nullptr && m_Type != nullptr; }

	private:
		void InvokeMethodInternal(std::string_view InMethodName, const void** InParameters, const ManagedType* InParameterTypes, size_t InLength) const;
		void InvokeMethodRetInternal(std::string_view InMethodName, const void** InParameters, const ManagedType* InParameterTypes, size_t InLength, void* InResultStorage) const;

	private:
		void* m_Handle = nullptr;
		const Type* m_Type = nullptr;

	private:
		friend class ManagedAssembly;
		friend class Type;
	};
	
}

