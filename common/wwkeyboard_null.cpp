#include "wwkeyboard.h"

class WWKeyboardClassNull : public WWKeyboardClass
{
public:
    virtual ~WWKeyboardClassNull()
    {
    }

    virtual KeyASCIIType To_ASCII(unsigned short num)
    {
        return (KeyASCIIType)num;
    }

    virtual void Fill_Buffer_From_System(void)
    {
    }
};

WWKeyboardClass* CreateWWKeyboardClass(void)
{
    return new WWKeyboardClassNull();
}
