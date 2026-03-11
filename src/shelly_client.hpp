#pragma once

class Database;
class DataStore;

namespace shelly {

// Turn relay on or off. Returns true on success.
bool turn_on(Database* db);
bool turn_off(Database* db);

// If battery test is active, check safety/auto-end conditions and turn grid on if needed.
// Call periodically (e.g. every 60s) from battery observer.
void check_test_conditions(Database* db, const DataStore& store);

} // namespace shelly
