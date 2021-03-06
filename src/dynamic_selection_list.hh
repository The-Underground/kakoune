#ifndef dynamic_selection_list_hh_INCLUDED
#define dynamic_selection_list_hh_INCLUDED

#include "selection.hh"

namespace Kakoune
{

class DynamicSelectionList : public SelectionList,
                             public BufferChangeListener_AutoRegister
{
public:
    using iterator = SelectionList::iterator;
    using const_iterator = SelectionList::const_iterator;

    DynamicSelectionList(Buffer& buffer, SelectionList selections = {});

    DynamicSelectionList& operator=(SelectionList selections);
    void check_invariant() const;

private:
    void on_insert(const Buffer& buffer, BufferCoord begin, BufferCoord end) override;
    void on_erase(const Buffer& buffer, BufferCoord begin, BufferCoord end) override;
};

}

#endif // dynamic_selection_list_hh_INCLUDED

