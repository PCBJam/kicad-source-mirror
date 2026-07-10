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
    // The MEMFS root the JS side stages official-lib model bodies under —
    // shared by the editor's models-bridge (constants.ts MODELS_3D_ROOT), the
    // occ_service export staging, and FindStagedModel below.
    inline constexpr char MODELS_MEMFS_ROOT[] = "/pcbjam/3dmodels";

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

    // Normalize a footprint model reference to its lib-relative form:
    // "${KICAD*_3DMODEL_DIR}/<lib>.3dshapes/<name>.<ext>" (any variable
    // vintage, brace or paren syntax) → "<lib>.3dshapes/<name>.<ext>"; bare
    // relative refs pass through; anything not served by the model libs
    // (absolute, ${KIPRJMOD}, URIs) → "".
    wxString NormalizeModelRef( const wxString& aModelRef );

    // Resolve a footprint model reference against files ALREADY staged under
    // MODELS_MEMFS_ROOT — a pure path probe, no JS bridge, usable where the
    // runtime cannot suspend (the occ_service worker's EXPORTER_STEP, built
    // -sASYNCIFY=0; bodies are staged up front by the export request there).
    // Tries the exact ref, then the same-stem format fallbacks (a .wrl ref is
    // served by its .step sibling — kicad-packages3D is STEP-only from 10.x —
    // and vice versa), mirroring the JS models-bridge refCandidates. Returns
    // the ABSOLUTE path of the staged file, "" on miss.
    wxString FindStagedModel( const wxString& aModelRef );
}

#endif // PCBJAM_MODEL_FETCH_H
