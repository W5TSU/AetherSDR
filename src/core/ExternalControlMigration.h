#pragma once

namespace AetherSDR {

// One-way migration of the legacy flat external-control settings keys
// (CatEnabled / CatPort_<n>_*, AutoStartTCI / TciPort, AutoStartDAX /
// DaxIqRate<n> / DaxIqEnabled<n>) into the nested CatSettings / TciSettings /
// DaxSettings objects. Runs once at startup, after migrateCatSettings() has
// normalised the oldest keys into CatPort_<n>_*. The legacy keys are left in
// place for downgrade safety. Idempotent.
namespace ExternalControlMigration {

// Returns true iff any nested object was written this call.
bool run();

} // namespace ExternalControlMigration

} // namespace AetherSDR
