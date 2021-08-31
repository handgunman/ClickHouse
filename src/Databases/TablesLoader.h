#pragma once
#include <Core/Types.h>
#include <Core/QualifiedTableName.h>
#include <Parsers/IAST_fwd.h>
#include <Interpreters/Context_fwd.h>
#include <Common/ThreadPool.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace Poco
{
    class Logger;
}

class AtomicStopwatch;

namespace DB
{

class IDatabase;
using DatabasePtr = std::shared_ptr<IDatabase>;

void logAboutProgress(Poco::Logger * log, size_t processed, size_t total, AtomicStopwatch & watch);

struct ParsedTablesMetadata
{
    String default_database;

    using ParsedMetadata = std::map<QualifiedTableName, std::pair<String, ASTPtr>>;
    std::mutex mutex;
    ParsedMetadata metadata;
    size_t total_dictionaries = 0;
    std::unordered_set<QualifiedTableName> independent_tables;
    std::unordered_map<QualifiedTableName, std::vector<QualifiedTableName>> table_dependencies;
};

class TablesLoader
{
public:

    using Databases = std::vector<DatabasePtr>;

    TablesLoader(ContextMutablePtr global_context_, Databases databases_, bool force_restore_ = false, bool force_attach_ = false);

    void loadTables();

private:
    ContextMutablePtr global_context;
    Databases databases;
    bool force_restore;
    bool force_attach;

    std::map<String, DatabasePtr> databases_to_load;
    ParsedTablesMetadata all_tables;
    Poco::Logger * log;
    std::atomic<size_t> tables_processed{0};


    using RemoveDependencyPredicate = std::function<bool(const QualifiedTableName &, const QualifiedTableName &)>;
    void removeDependencies(RemoveDependencyPredicate need_remove_dependency, std::unordered_set<QualifiedTableName> & independent_tables);

    void startLoadingIndependentTables(ThreadPool & pool, AtomicStopwatch & watch, size_t level);

    void checkCyclicDependencies() const;

};

}
