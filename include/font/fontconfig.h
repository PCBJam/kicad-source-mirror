/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2021 Ola Rinta-Koski
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

#ifndef KICAD_FONTCONFIG_H
#define KICAD_FONTCONFIG_H

// WASM builds don't have fontconfig (no system font access in browser)
#ifndef KICAD_USE_FONTCONFIG
    #if defined(__EMSCRIPTEN__)
        #define KICAD_USE_FONTCONFIG 0
    #else
        #define KICAD_USE_FONTCONFIG 1
    #endif
#endif

#if KICAD_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#include <kicommon.h>
#include <wx/string.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <font/fontinfo.h>

class REPORTER;
namespace fontconfig
{

struct FONTCONFIG_PAT;

class KICOMMON_API FONTCONFIG
{
public:
    FONTCONFIG();

    static wxString Version();

    enum class FF_RESULT
    {
        FF_OK,
        FF_ERROR,
        FF_SUBSTITUTE,
        FF_MISSING_BOLD,
        FF_MISSING_ITAL,
        FF_MISSING_BOLD_ITAL
    };

    /**
     * Given a fully-qualified font name ("Times:Bold:Italic") find the closest matching font
     * and return its filepath in \a aFontFile.
     *
     * A return value of false indicates a serious error in the font system.
     */
    FF_RESULT FindFont( const wxString& aFontName, wxString& aFontFile, int& aFaceIndex, bool aBold,
                        bool aItalic, const std::vector<wxString>* aEmbeddedFiles = nullptr );

    /**
     * List the current available font families.
     *
     * @param aDesiredLang The desired language of font name to report back if available,
     *                     otherwise it will fallback.
     * @param aEmbeddedFiles A list of embedded to use for searching fonts, if nullptr, this
     *                       is not used
     * @param aForce If true, force rebuilding the font cache
     */
    void ListFonts( std::vector<std::string>& aFonts, const std::string& aDesiredLang,
                    const std::vector<wxString>* aEmbeddedFiles = nullptr, bool aForce = false );

    /**
     * Set the reporter to use for reporting font substitution warnings.
     *
     * @param aReporter The reporter to use for reporting font substitution warnings.
     */
    static void SetReporter( REPORTER* aReporter );

    /**
     * Get the current reporter used for font substitution warnings.
     *
     * @return The current reporter, or nullptr if not set.
     */
    static REPORTER* GetReporter();

private:
    std::map<std::string, FONTINFO> m_fontInfoCache;
    wxString                        m_fontCacheLastLang;
    static REPORTER*                s_reporter;

#if KICAD_USE_FONTCONFIG
    /**
     * Match two rfc 3306 language entries, used for when searching for matching family names
     */
    bool isLanguageMatch( const wxString& aSearchLang, const wxString& aSupportedLang );

    /**
     * Get a list of all family name strings mapped to lang
     */
    void getAllFamilyStrings( FONTCONFIG_PAT&                               aPat,
                              std::unordered_map<std::string, std::string>& aFamStringMap );

    /**
     * Get a family name based on desired language.
     */
    std::string getFamilyStringByLang( FONTCONFIG_PAT& APat, const wxString& aDesiredLang );

    /**
     * Wrapper of FcPatternGetString to return a std::string
     */
    std::string getFcString( FONTCONFIG_PAT& aPat, const char* aObj, int aIdx );
#endif
};

} // namespace fontconfig


KICOMMON_API fontconfig::FONTCONFIG* Fontconfig();


#endif //KICAD_FONTCONFIG_H
