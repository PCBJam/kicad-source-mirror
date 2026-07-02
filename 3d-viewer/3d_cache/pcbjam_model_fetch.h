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

#ifndef PCBJAM_MODEL_FETCH_H
#define PCBJAM_MODEL_FETCH_H

#include <wx/string.h>

namespace PCBJAM_3D
{
    // Lazily materialize a footprint's referenced 3D model file in MEMFS and
    // return its ABSOLUTE path ("" when unavailable).
    //
    // Called by S3D_CACHE::load AFTER the stock resolver fails (so project-local
    // and already-resolvable refs never cross the bridge).  Normalizes an
    // official-lib style reference ("${KICAD*_3DMODEL_DIR}/<lib>.3dshapes/
    // <name>.<ext>") to its relative form and asks the JS provider
    // (kicadLibs.request, op "ensure", kind "model3d") to fetch the body, write
    // it under the JS-owned model root, and answer with the absolute MEMFS path
    // — which the cache then loads directly, independent of any env-var
    // expansion working inside the wasm runtime.
    //
    // Never throws and never re-crosses for repeat refs: results (the path, or
    // "" for a ref the provider can't serve) are memoized per session.
    wxString EnsureModelFile( const wxString& aModelRef );
}

#endif // PCBJAM_MODEL_FETCH_H
