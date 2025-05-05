#ifndef _PLAYERBOT_DUNGEONSTRATEGYCONTEXT_H
#define _PLAYERBOT_DUNGEONSTRATEGYCONTEXT_H

#include "Strategy.h"

class DungeonStrategyContext : public NamedObjectContext<Strategy>
{
    public:
        DungeonStrategyContext() : NamedObjectContext<Strategy>(false, true)
        {
            // Vanilla
            // ...

            // Burning Crusade
            // ...
            
            // Wrath of the Lich King
            // ...
        }
    private:
};

#endif
