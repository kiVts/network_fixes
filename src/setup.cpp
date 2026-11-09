// network_fixes: the builds its fixed addresses and struct offsets were checked against.
//
// 0x6ae218ec (Steam update 2026-10-28), checked with tools/port_*.py: hooks are at function starts or exact return
// addresses that map; the fields read are outside the blocks that moved. The main loop grew 48 bytes
// (map_catchup.cpp's range check).
#include "game.h"
#include "port_table.h"
#include "sdk.h"

namespace {
bool setup()
{
    mod_ported_to(kPortToBuild);
    return true;
}
ModSetup reg{&setup};
} // namespace
