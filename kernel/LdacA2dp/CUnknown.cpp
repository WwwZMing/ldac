// ============================================================================
//  CUnknown.cpp - stdunk.h 只声明 CUnknown，实现留给样例，这里补上
// ============================================================================
#include "LdacKs.h"

CUnknown::CUnknown(PUNKNOWN pUnknownOuter)
    : m_lRefCount(1),
      m_pUnknownOuter(pUnknownOuter)
{
}

CUnknown::~CUnknown(void)
{
}

STDMETHODIMP_(ULONG)
CUnknown::NonDelegatingAddRef(void)
{
    return (ULONG)InterlockedIncrement(&m_lRefCount);
}

STDMETHODIMP_(ULONG)
CUnknown::NonDelegatingRelease(void)
{
    LONG result = InterlockedDecrement(&m_lRefCount);

    if (result == 0) {
        delete this;
        return 0;
    }
    return (ULONG)result;
}

STDMETHODIMP_(NTSTATUS)
CUnknown::NonDelegatingQueryInterface(_In_ REFIID rIID, _COM_Outptr_ PVOID *ppVoid)
{
    if (ppVoid == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (IsEqualGUIDAligned(rIID, IID_IUnknown)) {
        //  被聚合时 IUnknown 由外层回答；我们是简单 miniport，outer 一直是 NULL
        if (m_pUnknownOuter != NULL) {
            *ppVoid = NULL;
            return STATUS_INVALID_PARAMETER;
        }
        *ppVoid = PVOID(PUNKNOWN(this));
        NonDelegatingAddRef();
        return STATUS_SUCCESS;
    }

    *ppVoid = NULL;
    return STATUS_INVALID_PARAMETER;
}
