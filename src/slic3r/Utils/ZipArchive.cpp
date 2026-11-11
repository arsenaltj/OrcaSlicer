#include "ZipArchive.hpp"
#include <wx/zipstrm.h>
#include <wx/wfstream.h>
#include <wx/filename.h>
#include <wx/dir.h>

namespace Slic3r::Diagnostics {
static bool add_to_zip(wxZipOutputStream& zip, const wxString& fullPathOrTextData, const wxString& rootDir, const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) return false;
    if (fullPathOrTextData.StartsWith(wxT("TxtData:"))) { // add text to zip
        // Format: "TxtData:<filename>|<content>"
        wxString payload = fullPathOrTextData.Mid(8); // strip "TxtData:"
        int sep = payload.Find('|');
        if (sep == wxNOT_FOUND)
            return false;
        wxString entryName = payload.Left(sep);
        wxString content   = payload.Mid(sep + 1);

        if (!zip.PutNextEntry(entryName))
            return false;
        wxScopedCharBuffer buf = content.utf8_str();
        zip.Write(buf.data(), buf.length());
        return zip.CloseEntry();
    }

    wxString relPath = fullPathOrTextData.Mid(rootDir.length());
    if (relPath.StartsWith(wxFileName::GetPathSeparator()))
        relPath = relPath.Mid(1);
    relPath.Replace(wxFileName::GetPathSeparator(), wxT("/"));
    if (wxDirExists(fullPathOrTextData)) {
        if (!relPath.IsEmpty()) {
            if (!relPath.EndsWith(wxT("/")))
                relPath += wxT("/");
            if (!zip.PutNextDirEntry(relPath))
                return false;
        }

        wxDir dir(fullPathOrTextData);
        if (!dir.IsOpened())
            return false;

        wxString filename;
        bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES | wxDIR_DIRS | wxDIR_HIDDEN);
        while (cont) {
            wxString childPath = fullPathOrTextData;
            if (!wxEndsWithPathSeparator(childPath))
                childPath += wxFileName::GetPathSeparator();
            childPath += filename;

            if (!add_to_zip(zip, childPath, rootDir, cancelled))
                return false;

            cont = dir.GetNext(&filename);
        }
    }
    else if (wxFileExists(fullPathOrTextData)) {
        wxFileInputStream in(fullPathOrTextData);
        if (!in.IsOk())
            return false;
        if (!zip.PutNextEntry(relPath))
            return false;
        char buffer[65536];
        while (in.IsOk() && !in.Eof()) {
            if (cancelled && cancelled()) return false;
            in.Read(buffer, sizeof(buffer));
            if (in.GetLastError() != wxSTREAM_NO_ERROR && in.GetLastError() != wxSTREAM_EOF) return false;
            zip.Write(buffer, in.LastRead());
            if (!zip.IsOk()) return false;
        }
        if (!zip.CloseEntry())
            return false;
    }
    else
        return false;

    return true;
}

bool save_zip_sources(const std::vector<wxString>& sourcePaths, const wxString& zipFullPath, const std::function<bool()>& cancelled)
{
    wxFileOutputStream out(zipFullPath);
    if (!out.IsOk())
        return false;
    wxZipOutputStream zip(out);
    if (!zip.IsOk()) {
        out.Close();
        return false;
    }
    bool success = true;
    for (const auto& sourcePath : sourcePaths) {
        if (sourcePath.StartsWith(wxT("TxtData:"))) {
            wxString rootDir; // unused for virtual entries
            if (!add_to_zip(zip, sourcePath, rootDir, cancelled)) {
                success = false;
                break;
            }
            continue;
        }
        if (!wxDirExists(sourcePath) && !wxFileExists(sourcePath)) {
            success = false;
            break;
        }
        wxString rootDir = wxFileName(sourcePath).GetPath();
        if (!wxEndsWithPathSeparator(rootDir))
            rootDir += wxFileName::GetPathSeparator();
        if (!add_to_zip(zip, sourcePath, rootDir, cancelled)) {
            success = false;
            break;
        }
    }
    if (!zip.Close()) success = false;
    if (!out.Close()) success = false;
    if (!success)
        wxRemoveFile(zipFullPath);
    return success;
}

}
