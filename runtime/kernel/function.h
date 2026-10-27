// Derived from Unleashed Recomp (https://github.com/hedge-dev/UnleashedRecomp),
// file UnleashedRecomp/kernel/function.h, commit
// cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c.
// Copyright (c) hedge-dev and contributors. License GPL-3.0 (see LICENSE and NOTICE).
//
// PPCContext <-> C++ argument marshalling for the host functions that
// implement the kernel imports (GUEST_FUNCTION_HOOK, used by
// KERNEL_EXPORT in registry.h).
//
// Changes from the original:
// - includes adapted to the runtime: XenonUtils' ppc_context.h and xbox.h (be<T>),
//   kernel/guest.h in place of memory.h; ppc_config.h is not needed (the kernel does not
//   use the image parameters);
// - g_memory.MapVirtual replaced by kernel::GuestAddr;
// - 8-byte integer parameters (uint64_t, int64_t) read the whole
//   register or the 8-byte stack slot (r1 + 0x50 + (i-8)*8); those up
//   to 4 bytes stay 32-bit as in the original (r1 + 0x54 + (i-8)*8);
// - GuestToHostFunction (call of guest code from the host) takes the
//   guest address and finds the function with kernel::FindGuestFunction
//   (function table, threads.cpp) instead of g_memory.FindFunction; the
//   current PPCContext is kernel::GetPPCContext (per thread, set by
//   GuestThread, like Unleashed's cpu/ppc_context.h); if the thread has no
//   context or the function does not exist the call is logged and returns
//   0; the new context is zeroed.
#pragma once

#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED
#endif
#include <ppc_context.h>
#include <xbox.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

#include "kernel/guest.h"
#include "kernel/ppc_layout.h"

// The PPCContext seen here must have gow2_kernel's layout (ppc_layout.h).
GOW2_CHECK_PPC_CONTEXT_LAYOUT();

template <typename R, typename... T>
constexpr std::tuple<T...> function_args(R(*)(T...)) noexcept
{
    return std::tuple<T...>();
}

template<auto V>
static constexpr decltype(V) constant_v = V;

template<typename T>
static constexpr bool is_precise_v = std::is_same_v<T, float> || std::is_same_v<T, double>;

template<auto Func>
struct arg_count_t
{
    static constexpr size_t value = std::tuple_size_v<decltype(function_args(Func))>;
};

template<typename TCallable, int I = 0, typename ...TArgs>
std::enable_if_t<(I >= sizeof...(TArgs)), void> _tuple_for(std::tuple<TArgs...>&, const TCallable& callable) noexcept
{

}

template<typename TCallable, int I = 0, typename ...TArgs>
std::enable_if_t<(I < sizeof...(TArgs)), void> _tuple_for(std::tuple<TArgs...>& tpl, const TCallable& callable) noexcept
{
    callable(std::get<I>(tpl), I);

    _tuple_for<TCallable, I + 1>(tpl, callable);
}

struct ArgTranslator
{
    constexpr static uint64_t GetIntegerArgumentValue(const PPCContext& ctx, uint8_t* base, size_t arg) noexcept
    {
        if (arg <= 7)
        {
            switch (arg)
            {
                case 0: return ctx.r3.u32;
                case 1: return ctx.r4.u32;
                case 2: return ctx.r5.u32;
                case 3: return ctx.r6.u32;
                case 4: return ctx.r7.u32;
                case 5: return ctx.r8.u32;
                case 6: return ctx.r9.u32;
                case 7: return ctx.r10.u32;
                default: break;
            }
        }

        // Guest stack: lives in the virtual heap (< 0xA0000000), no 0xE offset.
        return *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + ((arg - 8) * 8));
    }

    // Like GetIntegerArgumentValue, but for 8-byte parameters: the whole
    // integer register, or the 8-byte integer stack slot.
    static uint64_t GetInteger64ArgumentValue(const PPCContext& ctx, uint8_t* base, size_t arg) noexcept
    {
        switch (arg)
        {
            case 0: return ctx.r3.u64;
            case 1: return ctx.r4.u64;
            case 2: return ctx.r5.u64;
            case 3: return ctx.r6.u64;
            case 4: return ctx.r7.u64;
            case 5: return ctx.r8.u64;
            case 6: return ctx.r9.u64;
            case 7: return ctx.r10.u64;
            default: break;
        }

        // Guest stack (< 0xA0000000), as above.
        return *reinterpret_cast<be<uint64_t>*>(base + ctx.r1.u32 + 0x50 + ((arg - 8) * 8));
    }

    static double GetPrecisionArgumentValue(const PPCContext& ctx, uint8_t* base, size_t arg) noexcept
    {
        switch (arg)
        {
            case 0: return ctx.f1.f64;
            case 1: return ctx.f2.f64;
            case 2: return ctx.f3.f64;
            case 3: return ctx.f4.f64;
            case 4: return ctx.f5.f64;
            case 5: return ctx.f6.f64;
            case 6: return ctx.f7.f64;
            case 7: return ctx.f8.f64;
            case 8: return ctx.f9.f64;
            case 9: return ctx.f10.f64;
            case 10: return ctx.f11.f64;
            case 11: return ctx.f12.f64;
            case 12: return ctx.f13.f64;
            [[unlikely]] default: break;
        }

        // TODO: get value from stack.
        return 0;
    }

    constexpr static void SetIntegerArgumentValue(PPCContext& ctx, uint8_t* base, size_t arg, uint64_t value) noexcept
    {
        if (arg <= 7)
        {
            switch (arg)
            {
                case 0: ctx.r3.u64 = value; return;
                case 1: ctx.r4.u64 = value; return;
                case 2: ctx.r5.u64 = value; return;
                case 3: ctx.r6.u64 = value; return;
                case 4: ctx.r7.u64 = value; return;
                case 5: ctx.r8.u64 = value; return;
                case 6: ctx.r9.u64 = value; return;
                case 7: ctx.r10.u64 = value; return;
                [[unlikely]] default: break;
            }
        }

        assert(arg < 7 && "Pushing to stack memory is not yet supported.");
    }

    static void SetPrecisionArgumentValue(PPCContext& ctx, uint8_t* base, size_t arg, double value) noexcept
    {
        switch (arg)
        {
            case 0: ctx.f1.f64 = value; return;
            case 1: ctx.f2.f64 = value; return;
            case 2: ctx.f3.f64 = value; return;
            case 3: ctx.f4.f64 = value; return;
            case 4: ctx.f5.f64 = value; return;
            case 5: ctx.f6.f64 = value; return;
            case 6: ctx.f7.f64 = value; return;
            case 7: ctx.f8.f64 = value; return;
            case 8: ctx.f9.f64 = value; return;
            case 9: ctx.f10.f64 = value; return;
            case 10: ctx.f11.f64 = value; return;
            case 11: ctx.f12.f64 = value; return;
            case 12: ctx.f13.f64 = value; return;
            [[unlikely]] default: break;
        }

        assert(arg < 12 && "Pushing to stack memory is not yet supported.");
    }

    template<typename T>
    constexpr static std::enable_if_t<!std::is_pointer_v<T>, T> GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept
    {
        if constexpr (is_precise_v<T>)
        {
            return static_cast<T>(GetPrecisionArgumentValue(ctx, base, idx));
        }
        else if constexpr (sizeof(T) == 8)
        {
            return static_cast<T>(GetInteger64ArgumentValue(ctx, base, idx));
        }
        else
        {
            return static_cast<T>(GetIntegerArgumentValue(ctx, base, idx));
        }
    }

    template<typename T>
    constexpr static std::enable_if_t<std::is_pointer_v<T>, T> GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept
    {
        const auto v = GetIntegerArgumentValue(ctx, base, idx);
        if (!v)
        {
            return nullptr;
        }

        return reinterpret_cast<T>(base + gow2::HostOffset(static_cast<uint32_t>(v)));
    }

    template<typename T>
    constexpr static std::enable_if_t<!std::is_pointer_v<T>, void> SetValue(PPCContext& ctx, uint8_t* base, size_t idx, T value) noexcept
    {
        if constexpr (is_precise_v<T>)
        {
            SetPrecisionArgumentValue(ctx, base, idx, value);
        }
        else if constexpr (std::is_null_pointer_v<T>)
        {
            SetIntegerArgumentValue(ctx, base, idx, 0);
        }
        else if constexpr (std::is_pointer_v<T>)
        {
            SetIntegerArgumentValue(ctx, base, idx, kernel::GuestAddr(value));
        }
        else
        {
            SetIntegerArgumentValue(ctx, base, idx, value);
        }
    }

    template<typename T>
    constexpr static std::enable_if_t<std::is_pointer_v<T>, void> SetValue(PPCContext& ctx, uint8_t* base, size_t idx, T value) noexcept
    {
        const auto v = kernel::GuestAddr((void*)value);
        if (!v)
        {
            return;
        }

        SetValue(ctx, base, idx, v);
    }
};

struct Argument
{
    int type{};
    int ordinal{};
};

template<typename T1>
constexpr std::array<Argument, std::tuple_size_v<T1>> GatherFunctionArguments(const T1& tpl)
{
    std::array<Argument, std::tuple_size_v<T1>> args{};

    int floatOrdinal{};
    size_t i{};

    if constexpr (!args.empty())
    {
        std::apply([&](const auto& first, const auto&... rest)
            {
                auto append = [&]<typename T2>(const T2& v)
                {
                    if constexpr (is_precise_v<T2>)
                    {
                        args[i] = { 1, floatOrdinal++ };
                    }
                    else
                    {
                        args[i] = { 0, static_cast<int>(i) }; // integer arguments use their position
                    }

                    i++;
                };

                append(first);
                (append(rest), ...);
            }, tpl);
    }

    return args;
}

template<auto Func>
constexpr std::array<Argument, arg_count_t<Func>::value> GatherFunctionArguments()
{
    return GatherFunctionArguments(function_args(Func));
}

template<auto Func, size_t I>
struct arg_ordinal_t
{
    static constexpr size_t value = GatherFunctionArguments<Func>()[I].ordinal;
};

template<auto Func, int I = 0, typename ...TArgs>
void _translate_args_to_host(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>&) noexcept
    requires (I >= sizeof...(TArgs))
{
}

template <auto Func, int I = 0, typename ...TArgs>
std::enable_if_t<(I < sizeof...(TArgs)), void> _translate_args_to_host(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>& tpl) noexcept
{
    using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
    std::get<I>(tpl) = ArgTranslator::GetValue<T>(ctx, base, arg_ordinal_t<Func, I>::value);

    _translate_args_to_host<Func, I + 1>(ctx, base, tpl);
}

template<int I = 0, typename ...TArgs>
void _translate_args_to_guest(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>&) noexcept
    requires (I >= sizeof...(TArgs))
{
}

template <int I = 0, typename ...TArgs>
std::enable_if_t<(I < sizeof...(TArgs)), void> _translate_args_to_guest(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>& tpl) noexcept
{
    using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
    ArgTranslator::SetValue<T>(ctx, base, GatherFunctionArguments(std::tuple<TArgs...>{})[I].ordinal, std::get<I>(tpl));

    _translate_args_to_guest<I + 1>(ctx, base, tpl);
}

template<auto Func>
PPC_FUNC(HostToGuestFunction)
{
    using ret_t = decltype(std::apply(Func, function_args(Func)));

    auto args = function_args(Func);
    _translate_args_to_host<Func>(ctx, base, args);

    if constexpr (std::is_same_v<ret_t, void>)
    {
        std::apply(Func, args);
    }
    else
    {
        auto v = std::apply(Func, args);

        if constexpr (std::is_pointer<ret_t>())
        {
            if (v != nullptr)
            {
                ctx.r3.u64 = static_cast<uint32_t>(reinterpret_cast<size_t>(v) - reinterpret_cast<size_t>(base));
            }
            else
            {
                ctx.r3.u64 = 0;
            }
        }
        else if constexpr (is_precise_v<ret_t>)
        {
            ctx.f1.f64 = v;
        }
        else
        {
            ctx.r3.u64 = (uint64_t)v;
        }
    }
}

namespace kernel {
// PPCContext of the current guest thread (Unleashed's cpu/ppc_context.h):
// the one the thread started with, or that of a GuestToHostFunction call
// in progress. nullptr on host threads.
inline thread_local PPCContext* t_ppc_context = nullptr;
inline PPCContext* GetPPCContext() { return t_ppc_context; }
inline void SetPPCContext(PPCContext* ctx) { t_ppc_context = ctx; }

// Recompiled function for the guest address (nullptr if there is none), from the
// resolver set with SetGuestFunctionResolver (threads.h).
PPCFunc* FindGuestFunction(uint32_t guest);
namespace detail {
void LogGuestCallFailure(uint32_t guest, const char* why);
}  // namespace detail
}  // namespace kernel

// Calls guest function `guest` from the host on the current guest thread, with
// the arguments in r3.. (f1.. for float/double). The new context inherits
// r1 (stack), r13 (PCR) and fpscr from the current one. It is the only point from which
// the runtime enters guest code besides thread startup (APCs, timer
// routines, callbacks).
template<typename T = void, typename... TArgs>
T GuestToHostFunction(uint32_t guest, TArgs&&... argv)
{
    auto args = std::make_tuple(std::forward<TArgs>(argv)...);
    PPCContext* current = kernel::GetPPCContext();
    PPCFunc* func = kernel::FindGuestFunction(guest);
    if (!current || !func)
    {
        kernel::detail::LogGuestCallFailure(guest, !current ? "thread without guest context" : "function not recompiled");
        if constexpr (!std::is_void_v<T>)
            return T{};
        else
            return;
    }

    PPCContext newCtx{};
    newCtx.r1 = current->r1;
    newCtx.r13 = current->r13;
    newCtx.fpscr = current->fpscr;

    _translate_args_to_guest(newCtx, kernel::g_base, args);

    kernel::SetPPCContext(&newCtx);
    func(newCtx, kernel::g_base);
    current->fpscr = newCtx.fpscr;
    kernel::SetPPCContext(current);

    if constexpr (std::is_pointer_v<T>)
    {
        return kernel::GuestPtr<std::remove_pointer_t<T>>(newCtx.r3.u32);
    }
    else if constexpr (is_precise_v<T>)
    {
        return static_cast<T>(newCtx.f1.f64);
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return static_cast<T>(newCtx.r3.u64);
    }
    else
    {
        static_assert(std::is_void_v<T>, "Unsupported return type.");
    }
}

#define GUEST_FUNCTION_HOOK(subroutine, function) \
    PPC_FUNC(subroutine) { HostToGuestFunction<function>(ctx, base); }

#define GUEST_FUNCTION_STUB(subroutine) \
    PPC_FUNC(subroutine) { }
