#include "highlighter_registry.hh"

#include "exception.hh"
#include "window.hh"

namespace Kakoune
{

struct factory_not_found : public runtime_error
{
    factory_not_found(const std::string& name)
        : runtime_error("highlighter factory not found '" + name + "'") {}
};

void HighlighterRegistry::register_factory(const std::string& name,
                                           const HighlighterFactory& factory)
{
    assert(not m_factories.contains(name));
    m_factories.append(std::make_pair(name, factory));
}

void HighlighterRegistry::add_highlighter_to_window(Window& window,
                                                    const std::string& name,
                                                    const HighlighterParameters& parameters)
{
    auto it = m_factories.find(name);
    if (it == m_factories.end())
        throw factory_not_found(name);

    window.add_highlighter(it->second(window, parameters));
}

CandidateList HighlighterRegistry::complete_highlighter(const std::string& prefix,
                                                        size_t cursor_pos)
{
    return m_factories.complete_id<str_to_str>(prefix, cursor_pos);
}

}