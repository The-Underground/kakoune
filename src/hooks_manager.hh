#ifndef hooks_manager_hh_INCLUDED
#define hooks_manager_hh_INCLUDED

#include "context.hh"
#include "utils.hh"

#include <unordered_map>

namespace Kakoune
{

typedef std::function<void (const std::string&, const Context&)> HookFunc;

class HooksManager : public Singleton<HooksManager>
{
public:
    void add_hook(const std::string& hook_name, HookFunc hook);
    void run_hook(const std::string& hook_name, const std::string& param,
                  const Context& context) const;

private:
    std::unordered_map<std::string, std::vector<HookFunc>> m_hooks;
};

}

#endif // hooks_manager_hh_INCLUDED
