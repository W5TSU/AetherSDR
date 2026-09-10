#include "core/ExternalControlMigration.h"

#include "core/CatSettings.h"
#include "core/DaxSettings.h"
#include "core/TciSettings.h"

namespace AetherSDR {

bool ExternalControlMigration::run()
{
    bool wrote = false;
    wrote |= CatSettings::migrate();
    wrote |= TciSettings::migrate();
    wrote |= DaxSettings::migrate();
    return wrote;
}

} // namespace AetherSDR
