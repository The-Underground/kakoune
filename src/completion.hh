#ifndef completion_hh_INCLUDED
#define completion_hh_INCLUDED

#include "string.hh"

#include <vector>
#include <functional>

namespace Kakoune
{

class Context;

typedef std::vector<String> CandidateList;

struct Completions
{
    CandidateList candidates;
    ByteCount start;
    ByteCount end;

    Completions()
        : start(0), end(0) {}

    Completions(ByteCount start, ByteCount end)
        : start(start), end(end) {}
};

enum class CompletionFlags
{
    None,
    Fast
};
using Completer = std::function<Completions (const Context&, CompletionFlags,
                                             const String&, ByteCount)>;

inline Completions complete_nothing(const Context& context, CompletionFlags,
                                    const String&, ByteCount cursor_pos)
{
    return Completions(cursor_pos, cursor_pos);
}

Completions shell_complete(const Context& context, CompletionFlags,
                           const String&, ByteCount cursor_pos);

}
#endif // completion_hh_INCLUDED
