#pragma once

#include "chatmodel.h"
#include "database.h"

// FIXME(jared): Jinja2Cpp headers should compile with -Werror=undef
#ifdef __GNUC__
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wundef"
#endif
#include <jinja2cpp/value.h>
#ifdef __GNUC__
#   pragma GCC diagnostic pop
#endif

#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace views = std::views;


template <typename T>
using JinjaFieldMap = std::unordered_map<std::string_view, std::function<jinja2::Value (const T &)>>;

template <typename Derived>
struct JinjaComparable : jinja2::IMapItemAccessor {
    bool IsEqual(const jinja2::IComparable &other) const override;
};

template <typename Derived>
struct JinjaHelper : JinjaComparable<Derived> {
    size_t GetSize() const override
    { return Derived::s_fields.size(); }

    bool HasValue(const std::string &name) const override
    { return Derived::s_fields.contains(name); }

    jinja2::Value GetValueByName(const std::string &name) const override;

    std::vector<std::string> GetKeys() const override
    { auto keys = views::elements<0>(Derived::s_fields); return { keys.begin(), keys.end() }; }
};

class JinjaResultInfo : public JinjaHelper<JinjaResultInfo> {
public:
    explicit JinjaResultInfo(const ResultInfo &source) noexcept
        : m_source(&source) {}

    const ResultInfo &value() const { return *m_source; }

    friend bool operator==(const JinjaResultInfo &a, const JinjaResultInfo &b) { return a.m_source == b.m_source; }

private:
    static const JinjaFieldMap<ResultInfo> s_fields;
    const ResultInfo *m_source;

    friend class JinjaHelper<JinjaResultInfo>;
};

class JinjaPromptAttachment : public JinjaHelper<JinjaPromptAttachment> {
public:
    explicit JinjaPromptAttachment(const PromptAttachment &attachment) noexcept
        : m_attachment(&attachment) {}

    const PromptAttachment &value() const { return *m_attachment; }

    friend bool operator==(const JinjaPromptAttachment &a, const JinjaPromptAttachment &b)
    { return a.m_attachment == b.m_attachment; }

private:
    static const JinjaFieldMap<PromptAttachment> s_fields;
    const PromptAttachment *m_attachment;

    friend class JinjaHelper<JinjaPromptAttachment>;
};

class JinjaMessage : public JinjaHelper<JinjaMessage> {
private:
    enum class Role { User, Assistant };

public:
    explicit JinjaMessage(const ChatItem &item) noexcept
        : m_item(&item) {}

    const ChatItem     &item () const { return *m_item; }
    const JinjaMessage &value() const { return *this;   }

    size_t GetSize() const override { return keys().size(); }
    bool HasValue(const std::string &name) const override { return keys().contains(name); }

    jinja2::Value GetValueByName(const std::string &name) const override
    { return HasValue(name) ? JinjaHelper::GetValueByName(name) : jinja2::EmptyValue(); }

    std::vector<std::string> GetKeys() const override;

    friend bool operator==(const JinjaMessage &a, const JinjaMessage &b) { return a.m_item == b.m_item; }

private:
    Role role() const;
    auto keys() const -> const std::unordered_set<std::string_view> &;

private:
    static const JinjaFieldMap<JinjaMessage> s_fields;
    const ChatItem *m_item;

    friend class JinjaHelper<JinjaMessage>;
};

#include "jinja_helpers.inl"
