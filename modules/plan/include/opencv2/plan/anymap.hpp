// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_ANYMAP_HPP_
#define OPENCV_PLAN_ANYMAP_HPP_

#include "defs.hpp"
#include <any>
#include <vector>
#include <mutex>
#include <functional>
#include <string>
#include <memory>
#include <typeinfo>

namespace plan {

class PLAN_EXPORTS Value : public std::any {
public:
    std::function<void(Value& val)> callback_;
    const bool read_;

    Value(const bool& read = false) : read_(read) {}

    any& operator=(const any& rhs) { return std::any::operator=(rhs); }

    template <typename T>
    any& operator=(T&& __rhs) {
        std::any::operator=(any(std::forward<T>(__rhs)));
        return *this;
    }
};

template<typename K>
class PLAN_EXPORTS AnyPropertyMap {
private:
    std::vector<Value> properties_;

    template<typename V>
    constexpr void check_value_type() const {
        using U = std::remove_cv_t<std::remove_reference_t<V>>;
        static_assert(std::is_constructible<V, const U&>::value,
                      "Illegal value type: Can't construct const V&");
        static_assert(std::is_constructible<V, U&>::value,
                      "Illegal value type: Can't construct V&");
        static_assert(std::is_constructible<V, U>::value,
                      "Illegal value type: Can't construct V from itself");
        static_assert(!std::is_void<V>::value,
                      "Illegal value type: V may not be void");
    }

    void check_write(K key) {
        PLAN_Assert(properties_.size() > static_cast<size_t>(key));
        if (properties_[static_cast<size_t>(key)].read_) {
            throw std::runtime_error("You are trying to set a read only property");
        }
    }

    static_assert(std::is_enum<K>::value);

public:
    AnyPropertyMap() { properties_.reserve(100); }

    template<bool Tread, typename V>
    void create(K key, const V& value, std::function<void(const V& val)> cb) {
        check_value_type<V>();
        PLAN_Assert(properties_.size() == static_cast<size_t>(key));
        PLAN_Assert(!Tread || (Tread && !cb));
        if constexpr (Tread) {
            Value val(Tread);
            val.callback_ = [](const Value& v){ PLAN_UNUSED(v); };
            val = value;
            properties_.emplace_back(val);
        } else {
            if (!cb) cb = [](const V& v){ PLAN_UNUSED(v); };
            properties_.emplace_back(Value(Tread));
            properties_[static_cast<size_t>(key)] = value;
            Value& val = properties_[static_cast<size_t>(key)];
            val.callback_ = [cb](const Value& v){ cb(std::any_cast<V>(v)); };
        }
    }

    template<typename V>
    void set(K key, const V& value, bool fire = true) {
        check_value_type<V>();
        check_write(key);
        V* p = std::any_cast<V>(&properties_[static_cast<size_t>(key)]);
        if (!p)
            throw std::invalid_argument(
                std::string("Type mismatch for key: ") +
                std::to_string(int(key)) + ". Expected: " +
                detail::demangle(properties_[static_cast<size_t>(key)].type().name()) +
                ", Got: " + detail::type_name<V>() + ".");
        V oldVal = *p;
        *p = value;
        if (fire && std::memcmp(&oldVal, p, sizeof(V)) != 0)
            properties_[static_cast<size_t>(key)].callback_(
                properties_[static_cast<size_t>(key)]);
    }

    template<typename V>
    constexpr const V& get(K key) const {
        check_value_type<V>();
        return *ptr<V>(key);
    }

    template<typename V>
    auto apply(K key, std::function<V(V&)> func) {
        check_value_type<V>();
        check_write(key);
        return func(*std::any_cast<V>(&properties_[static_cast<size_t>(key)]));
    }

    template<typename V>
    constexpr const V* ptr(K key) const {
        check_value_type<V>();
        return std::any_cast<V>(&properties_[static_cast<size_t>(key)]);
    }

    size_t size() const { return properties_.size(); }
    bool empty() const { return properties_.empty(); }
};

template<typename K>
class PLAN_EXPORTS ThreadSafeAnyMap : public AnyPropertyMap<K> {
private:
    std::mutex mtx_;
    using parent_t = AnyPropertyMap<K>;

public:
    template<bool Tread, typename V>
    void create(K key, const V& value,
                const std::function<void(const V& val)>& cb =
                    std::function<void(const V& val)>()) {
        std::unique_lock lock(mtx_);
        parent_t::template create<Tread>(key, value, cb);
    }

    template<typename V>
    void set(K key, const V& value, bool fire = true) {
        std::unique_lock lock(mtx_);
        parent_t::set(key, value, fire);
    }

    template<typename V>
    const V& get(K key) {
        std::unique_lock lock(mtx_);
        return parent_t::template get<V>(key);
    }

    template<typename V>
    V apply(K key, std::function<V(V&)> func) {
        std::unique_lock lock(mtx_);
        return parent_t::template apply<V>(key, func);
    }

    template<typename V>
    V* ptr(K key) const {
        return parent_t::template ptr<V>(key);
    }
};

} // namespace plan

#endif // OPENCV_PLAN_ANYMAP_HPP_

