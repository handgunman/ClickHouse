#pragma once

#include <functional>
#include <memory>
#include <utility>

template <class F, bool Noexcept = true>
class [[nodiscard]] BasicScopeGuard
{
public:
    constexpr BasicScopeGuard() = default;
    constexpr BasicScopeGuard(BasicScopeGuard && src) : function{src.release()} {} // NOLINT(hicpp-noexcept-move, performance-noexcept-move-constructor, cppcoreguidelines-noexcept-move-operations)

    constexpr BasicScopeGuard & operator=(BasicScopeGuard && src) // NOLINT(hicpp-noexcept-move, performance-noexcept-move-constructor, cppcoreguidelines-noexcept-move-operations)
    {
        if (this != &src)
        {
            invoke();
            function = src.release();
        }
        return *this;
    }

    template <typename G>
    requires std::is_convertible_v<G, F>
    constexpr BasicScopeGuard(BasicScopeGuard<G> && src) : function{src.release()} {} // NOLINT(google-explicit-constructor, cppcoreguidelines-rvalue-reference-param-not-moved, cppcoreguidelines-noexcept-move-operations)

    template <typename G>
    requires std::is_convertible_v<G, F>
    constexpr BasicScopeGuard & operator=(BasicScopeGuard<G> && src) // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved, cppcoreguidelines-noexcept-move-operations)
    {
        if constexpr (std::is_same_v<G, F>)
        {
            if (this == &src)
                return *this;
        }
        invoke();
        function = src.release();
        return *this;
    }

    template <typename G>
    requires std::is_convertible_v<G, F>
    constexpr BasicScopeGuard(const G & function_) : function{function_} {} // NOLINT(google-explicit-constructor)

    template <typename G>
    requires std::is_convertible_v<G, F>
    constexpr BasicScopeGuard(G && function_) : function{std::move(function_)} {} // NOLINT(google-explicit-constructor, bugprone-forwarding-reference-overload, bugprone-move-forwarding-reference, cppcoreguidelines-missing-std-forward)

    ~BasicScopeGuard() noexcept(Noexcept) { invoke(); }

    static constexpr bool is_nullable = std::is_constructible_v<bool, F>;

    explicit operator bool() const
    {
        if constexpr (is_nullable)
            return static_cast<bool>(function);
        return true;
    }

    void reset()
    {
        invoke();
        release();
    }

    F release()
    {
        static_assert(is_nullable);
        return std::exchange(function, {});
    }

    template <typename G>
    requires std::is_convertible_v<G, F>
    BasicScopeGuard<F> & join(BasicScopeGuard<G> && other) // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
    {
        if (other.function)
        {
            if (function)
            {
                function = [x = std::make_shared<std::pair<F, G>>(std::move(function), other.release())]()
                {
                    std::move(x->first)();
                    std::move(x->second)();
                };
            }
            else
                function = other.release();
        }
        return *this;
    }

private:
    void invoke()
    {
        if constexpr (is_nullable)
        {
            if (!function)
                return;
        }
        std::move(function)();
    }

    F function = F{};
};

using scope_guard = BasicScopeGuard<std::function<void(void)>>;


/// SCOPE_EXIT has noexcept(true) destructor, i.e. the program will terminate if an exception is thrown
/// out of the body.

template <class F>
inline BasicScopeGuard<F> make_scope_guard(F && function_) { return std::forward<F>(function_); }

#define SCOPE_EXIT_CONCAT(n, ...) \
const auto scope_exit##n = make_scope_guard([&] { __VA_ARGS__; })
#define SCOPE_EXIT_FWD(n, ...) SCOPE_EXIT_CONCAT(n, __VA_ARGS__)
#define SCOPE_EXIT(...) SCOPE_EXIT_FWD(__LINE__, __VA_ARGS__)

/// SCOPE_EXIT_MAY_THROW has noexcept(false) destructor, i.e. throwing an exception out of the body
/// is ok unless there's already another uncaught exception (std::uncaught_exceptions() != 0, which means
/// this destructor was called during stack unwinding caused by an exception).

template <class F>
inline BasicScopeGuard<F, false> make_scope_guard_may_throw(F && function_) { return std::forward<F>(function_); }

#define SCOPE_EXIT_CONCAT_MAY_THROW(n, ...) \
const auto scope_exit##n = make_scope_guard_may_throw([&] { __VA_ARGS__; })
#define SCOPE_EXIT_FWD_MAY_THROW(n, ...) SCOPE_EXIT_CONCAT_MAY_THROW(n, __VA_ARGS__)
#define SCOPE_EXIT_MAY_THROW(...) SCOPE_EXIT_FWD_MAY_THROW(__LINE__, __VA_ARGS__)
