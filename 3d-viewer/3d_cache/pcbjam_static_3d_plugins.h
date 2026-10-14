/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PCBJAM_STATIC_3D_PLUGINS_H
#define PCBJAM_STATIC_3D_PLUGINS_H

#include <list>
#include <wx/string.h>

// WASM has no dlopen, so the 3D format plugins (upstream: MODULE DSOs found by a
// directory scan) are linked statically with per-plugin symbol prefixes and exposed
// through this registry.  S3D_PLUGIN_MANAGER::loadPlugins feeds StaticPluginPaths()
// through its normal Open/registration flow, and pluginldr.h's LINK_ITEM resolves
// entry points via StaticPluginSymbol instead of wxDynamicLibrary::GetSymbol.
namespace PCBJAM_3D
{
    // Pseudo-paths ("static://vrml", "static://oce") standing in for plugin files.
    std::list<wxString> StaticPluginPaths();

    // Resolve an entry point ("Load", "GetNExtensions", ...) of the plugin behind
    // aPluginPath.  Returns nullptr for unknown plugins/symbols, mirroring a failed
    // GetSymbol.
    void* StaticPluginSymbol( const wxString& aPluginPath, const char* aSymbolName );
}

#endif // PCBJAM_STATIC_3D_PLUGINS_H
