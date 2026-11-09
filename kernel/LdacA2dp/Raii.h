// ============================================================================
//  Raii.h - 内核里唯一允许的资源管理方式（2026-11-08 起强制）
// ============================================================================
//  规则：禁止手写 Acquire/Release、Allocate/Free 配对。
//  只要是「拿到 -> 用完 -> 还回去」的东西，就必须走这里的守卫类型，
//  由析构器负责归还；确实要交出所有权时用 Detach()，并写清楚为什么。
//
//  为什么：本驱动历史上的崩溃都来自「某条路径忘了还 / 还早了」——
//    0x139（事件被重复初始化后又被摘链表）、0xC9/0x35（PDO 悬空）、
//    0x3B（注销 KS 子设备与 create 抢跑）。裸指针 + 手写释放无法表达
//    「谁在什么时候还」，所以干脆不让写。
//
//  注意：只能被 C++ 翻译单元 include（Driver.cpp/KsAdapter.cpp/BthL2cap.cpp）。
//  Avdtp.c 与 Log.c 保持纯 C —— 它们不拥有任何资源。
// ============================================================================
#pragma once

#include <ntddk.h>

// ---------------------------------------------------------------- 作用域退出
//  通用兜底：把任意清理动作绑到作用域上。
template <typename Fn>
class ScopeExit final {
public:
    explicit ScopeExit(Fn fn) noexcept : fn_(fn) {}
    ~ScopeExit() { if (active_) fn_(); }

    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    //  放弃这次清理（所有权已经交给别人）。
    void Dismiss() noexcept { active_ = FALSE; }

private:
    Fn      fn_;
    BOOLEAN active_ = TRUE;
};

template <typename Fn>
ScopeExit<Fn> MakeScopeExit(Fn fn) noexcept { return ScopeExit<Fn>(fn); }

// ---------------------------------------------------------------- ERESOURCE
enum class LockMode { Shared, Exclusive };

class ResourceGuard final {
public:
    ResourceGuard(_In_ PERESOURCE Resource, _In_ LockMode Mode) noexcept
        : res_(Resource)
    {
        if (Mode == LockMode::Shared) ExAcquireResourceSharedLite(res_, TRUE);
        else                          ExAcquireResourceExclusiveLite(res_, TRUE);
    }

    ~ResourceGuard() { ExReleaseResourceLite(res_); }

    ResourceGuard(const ResourceGuard&)            = delete;
    ResourceGuard& operator=(const ResourceGuard&) = delete;

private:
    PERESOURCE res_;
};

// -------------------------------------------------------------------- IRP
class IrpGuard final {
public:
    explicit IrpGuard(_In_opt_ PIRP Irp) noexcept : irp_(Irp) {}
    ~IrpGuard() { if (irp_ != nullptr) IoFreeIrp(irp_); }

    IrpGuard(const IrpGuard&)            = delete;
    IrpGuard& operator=(const IrpGuard&) = delete;

    PIRP Get() const noexcept { return irp_; }

    //  交出所有权（例如完成例程还没跑完，此时 free 会踩到协议栈）。
    PIRP Detach() noexcept { PIRP p = irp_; irp_ = nullptr; return p; }

private:
    PIRP irp_;
};

// ------------------------------------------------------------------- 池对象
class PoolGuard final {
public:
    PoolGuard(_In_opt_ PVOID Mem, _In_ ULONG Tag) noexcept : mem_(Mem), tag_(Tag) {}
    ~PoolGuard() { if (mem_ != nullptr) ExFreePoolWithTag(mem_, tag_); }

    PoolGuard(const PoolGuard&)            = delete;
    PoolGuard& operator=(const PoolGuard&) = delete;

    PVOID Get() const noexcept { return mem_; }

    template <typename T> T* As() const noexcept { return static_cast<T*>(mem_); }

    PVOID Detach() noexcept { PVOID p = mem_; mem_ = nullptr; return p; }

private:
    PVOID mem_;
    ULONG tag_;
};

// ------------------------------------------------------------------ 对象引用
//  别人的对象（PDO 等）：只能以「我持有一份引用」的方式保存。
//  这就是 0xC9/0x35 那类悬空指针的正解：对象即使被 PnP 删掉，
//  只要我们还持有引用，内存就不会先失效。
class ObjectRef final {
public:
    explicit ObjectRef(_In_opt_ PVOID Object) noexcept : obj_(Object)
    {
        if (obj_ != nullptr) ObReferenceObject(obj_);
    }

    ~ObjectRef() { if (obj_ != nullptr) ObDereferenceObject(obj_); }

    ObjectRef(const ObjectRef&)            = delete;
    ObjectRef& operator=(const ObjectRef&) = delete;

    ObjectRef(ObjectRef&& other) noexcept : obj_(other.obj_) { other.obj_ = nullptr; }

    PVOID Get() const noexcept { return obj_; }
    template <typename T> T* As() const noexcept { return static_cast<T*>(obj_); }

    PVOID Detach() noexcept { PVOID p = obj_; obj_ = nullptr; return p; }

private:
    PVOID obj_;
};

// ------------------------------------------------------------- COM 式引用计数
template <typename T>
class ComRef final {
public:
    explicit ComRef(_In_opt_ T* Obj) noexcept : obj_(Obj) {}
    ~ComRef() { if (obj_ != nullptr) obj_->Release(); }

    ComRef(const ComRef&)            = delete;
    ComRef& operator=(const ComRef&) = delete;

    ComRef(ComRef&& other) noexcept : obj_(other.obj_) { other.obj_ = nullptr; }

    T* Get() const noexcept { return obj_; }
    T* operator->() const noexcept { return obj_; }

    T* Detach() noexcept { T* p = obj_; obj_ = nullptr; return p; }

private:
    T* obj_;
};