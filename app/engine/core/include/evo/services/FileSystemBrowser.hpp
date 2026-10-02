#ifndef EVO_FILE_SYSTEM_BROWSER_HPP
#define EVO_FILE_SYSTEM_BROWSER_HPP

#include "evo/interfaces/IFileSystemBrowser.hpp"
#include <string>
#include <vector>

namespace evo {

class FileSystemBrowser : public IFileSystemBrowser {
public:
    FileSystemBrowser();
    ~FileSystemBrowser() override = default;

    const std::vector<MediaSourceInfo>& getSources() const override { return m_sources; }
    const std::string& getCurrentPath() const override { return m_currentPath; }
    void setCurrentPath(const std::string& path) override;

    bool navigateToSource(size_t sourceIndex) override;
    bool navigateInto(const std::string& folderName) override;
    bool navigateUp() override;
    bool refresh() override;

    const std::vector<BrowserEntry>& getEntries() const override { return m_entries; }
    size_t getEntryCount() const override { return m_entries.size(); }
    const BrowserEntry* getEntry(size_t index) const override;

    void search(const std::string& query) override;
    void clearSearch() override;
    bool isSearching() const override { return !m_searchQuery.empty(); }
    const std::string& getSearchQuery() const override { return m_searchQuery; }

    void saveLastFolder() override;
    void loadLastFolder() override;

    std::string getFullPath(size_t index) const override;
    FileCategory classifyFile(const std::string& fileName, int entryType) const override;
    const char* getFileCategoryLabel(FileCategory category) const override;

    void setSortFoldersFirst(bool enabled) { m_sortFoldersFirst = enabled; }

    /*
     * Library view: collect every file of one category under the current
     * source, recursively, instead of listing one directory.
     *
     * The sidebar's "All Videos" / "All Music" / "All Photos" were filtering
     * the folder you happened to be standing in and keeping every subfolder,
     * so at a source root they showed the same folders as before and none of
     * the media underneath. FileCategory::Unknown turns the mode off.
     */

private:
    void scanDirectory(const std::string& dirPath);
    void scanRecursive(const std::string& basePath, const std::string& relPath,
                       const std::string& query, int depth, const std::string& sourcePrefix = "");
    bool isSafePath(const std::string& path) const;

    std::vector<MediaSourceInfo> m_sources;
    std::string m_currentPath;
    std::string m_searchQuery;
    std::vector<BrowserEntry> m_entries;
    bool m_sortFoldersFirst = true;
};

} // namespace evo

#endif // EVO_FILE_SYSTEM_BROWSER_HPP
