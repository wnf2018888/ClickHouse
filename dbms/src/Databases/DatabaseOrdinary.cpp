#include <iomanip>

#include <Core/Settings.h>
#include <Databases/DatabaseOnDisk.h>
#include <Databases/DatabaseOrdinary.h>
#include <Databases/DatabasesCommon.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterCreateQuery.h>
#include <Interpreters/ExternalLoaderDatabaseConfigRepository.h>
#include <Interpreters/ExternalDictionariesLoader.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ParserCreateQuery.h>
#include <Storages/StorageFactory.h>
#include <Parsers/parseQuery.h>
#include <Parsers/formatAST.h>
#include <Parsers/ASTSetQuery.h>
#include <TableFunctions/TableFunctionFactory.h>

#include <Parsers/queryToString.h>

#include <Poco/DirectoryIterator.h>
#include <Poco/Event.h>
#include <Common/Stopwatch.h>
#include <Common/quoteString.h>
#include <Common/ThreadPool.h>
#include <Common/escapeForFileName.h>
#include <Common/typeid_cast.h>
#include <common/logger_useful.h>
#include <ext/scope_guard.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_CREATE_TABLE_FROM_METADATA;
    extern const int CANNOT_CREATE_DICTIONARY_FROM_METADATA;
    extern const int EMPTY_LIST_OF_COLUMNS_PASSED;
    extern const int CANNOT_PARSE_TEXT;
}


static constexpr size_t PRINT_MESSAGE_EACH_N_OBJECTS = 256;
static constexpr size_t PRINT_MESSAGE_EACH_N_SECONDS = 5;
static constexpr size_t METADATA_FILE_BUFFER_SIZE = 32768;


namespace
{
    void tryAttachTable(
        Context & context,
        const ASTCreateQuery & query,
        DatabaseOrdinary & database,
        const String & database_name,
        bool has_force_restore_data_flag)
    {
        assert(!query.is_dictionary);
        try
        {
            String table_name;
            StoragePtr table;
            std::tie(table_name, table)
                = createTableFromAST(query, database_name, database.getTableDataPath(query), context, has_force_restore_data_flag);
            database.attachTable(table_name, table);
        }
        catch (const Exception & e)
        {
            throw Exception(
                "Cannot attach table '" + query.table + "' from query " + serializeAST(query)
                    + ". Error: " + DB::getCurrentExceptionMessage(true),
                e,
                DB::ErrorCodes::CANNOT_CREATE_TABLE_FROM_METADATA);
        }
    }


    void tryAttachDictionary(
        Context & context,
        const ASTCreateQuery & query,
        DatabaseOrdinary & database)
    {
        assert(query.is_dictionary);
        try
        {
            database.attachDictionary(query.table, context);
        }
        catch (const Exception & e)
        {
            throw Exception(
                "Cannot create dictionary '" + query.table + "' from query " + serializeAST(query)
                    + ". Error: " + DB::getCurrentExceptionMessage(true),
                e,
                DB::ErrorCodes::CANNOT_CREATE_DICTIONARY_FROM_METADATA);
        }
    }


    void logAboutProgress(Poco::Logger * log, size_t processed, size_t total, AtomicStopwatch & watch)
    {
        if (processed % PRINT_MESSAGE_EACH_N_OBJECTS == 0 || watch.compareAndRestart(PRINT_MESSAGE_EACH_N_SECONDS))
        {
            LOG_INFO(log, std::fixed << std::setprecision(2) << processed * 100.0 / total << "%");
            watch.restart();
        }
    }
}


DatabaseOrdinary::DatabaseOrdinary(const String & name_, const String & metadata_path_, const Context & context_)
    : DatabaseWithDictionaries(name_, metadata_path_, "DatabaseOrdinary (" + name_ + ")")
{
    Poco::File(context_.getPath() + getDataPath()).createDirectories();
}


void DatabaseOrdinary::loadStoredObjects(
    Context & context,
    bool has_force_restore_data_flag)
{

    /** Tables load faster if they are loaded in sorted (by name) order.
      * Otherwise (for the ext4 filesystem), `DirectoryIterator` iterates through them in some order,
      *  which does not correspond to order tables creation and does not correspond to order of their location on disk.
      */
    using FileNames = std::map<std::string, ASTPtr>;
    FileNames file_names;

    size_t total_dictionaries = 0;
    iterateMetadataFiles(context, [&file_names, &total_dictionaries, this](const String & file_name)
    {
        String full_path = getMetadataPath() + file_name;
        try
        {
            auto ast = parseQueryFromMetadata(full_path, /*throw_on_error*/ true, /*remove_empty*/false);
            if (ast)
            {
                auto * create_query = ast->as<ASTCreateQuery>();
                file_names[file_name] = ast;
                total_dictionaries += create_query->is_dictionary;
            }
        }
        catch (const Exception & e)
        {
            throw Exception(
                "Cannot parse definition from metadata file " + full_path + ". Error: " + DB::getCurrentExceptionMessage(true), e, ErrorCodes::CANNOT_PARSE_TEXT);
        }

    });

    size_t total_tables = file_names.size() - total_dictionaries;

    LOG_INFO(log, "Total " << total_tables << " tables and " << total_dictionaries << " dictionaries.");

    AtomicStopwatch watch;
    std::atomic<size_t> tables_processed{0};
    std::atomic<size_t> dictionaries_processed{0};

    ThreadPool pool(SettingMaxThreads().getAutoValue());

    /// Attach tables.
    for (const auto & name_with_query : file_names)
    {
        const auto & create_query = name_with_query.second->as<const ASTCreateQuery &>();
        if (!create_query.is_dictionary)
            pool.scheduleOrThrowOnError([&]()
            {
                tryAttachTable(context, create_query, *this, getDatabaseName(), has_force_restore_data_flag);

                /// Messages, so that it's not boring to wait for the server to load for a long time.
                logAboutProgress(log, ++tables_processed, total_tables, watch);
            });
    }

    pool.wait();

    /// After all tables was basically initialized, startup them.
    startupTables(pool);

    /// Add database as repository
    auto dictionaries_repository = std::make_unique<ExternalLoaderDatabaseConfigRepository>(shared_from_this(), context);
    auto & external_loader = context.getExternalDictionariesLoader();
    external_loader.addConfigRepository(getDatabaseName(), std::move(dictionaries_repository));

    /// Attach dictionaries.
    for (const auto & name_with_query : file_names)
    {
        auto create_query = name_with_query.second->as<const ASTCreateQuery &>();
        if (create_query.is_dictionary)
        {
            tryAttachDictionary(context, create_query, *this);

            /// Messages, so that it's not boring to wait for the server to load for a long time.
            logAboutProgress(log, ++dictionaries_processed, total_dictionaries, watch);
        }
    }
}


void DatabaseOrdinary::startupTables(ThreadPool & thread_pool)
{
    LOG_INFO(log, "Starting up tables.");

    const size_t total_tables = tables.size();
    if (!total_tables)
        return;

    AtomicStopwatch watch;
    std::atomic<size_t> tables_processed{0};

    auto startupOneTable = [&](const StoragePtr & table)
    {
        table->startup();
        logAboutProgress(log, ++tables_processed, total_tables, watch);
    };

    try
    {
        for (const auto & table : tables)
            thread_pool.scheduleOrThrowOnError([&]() { startupOneTable(table.second); });
    }
    catch (...)
    {
        thread_pool.wait();
        throw;
    }
    thread_pool.wait();
}

void DatabaseOrdinary::alterTable(
    const Context & context,
    const String & table_name,
    const StorageInMemoryMetadata & metadata)
{
    /// Read the definition of the table and replace the necessary parts with new ones.
    String table_metadata_path = getObjectMetadataPath(table_name);
    String table_metadata_tmp_path = table_metadata_path + ".tmp";
    String statement;

    {
        char in_buf[METADATA_FILE_BUFFER_SIZE];
        ReadBufferFromFile in(table_metadata_path, METADATA_FILE_BUFFER_SIZE, -1, in_buf);
        readStringUntilEOF(statement, in);
    }

    ParserCreateQuery parser;
    ASTPtr ast = parseQuery(parser, statement.data(), statement.data() + statement.size(), "in file " + table_metadata_path, 0);

    const auto & ast_create_query = ast->as<ASTCreateQuery &>();

    ASTPtr new_columns = InterpreterCreateQuery::formatColumns(metadata.columns);
    ASTPtr new_indices = InterpreterCreateQuery::formatIndices(metadata.indices);
    ASTPtr new_constraints = InterpreterCreateQuery::formatConstraints(metadata.constraints);

    ast_create_query.columns_list->replace(ast_create_query.columns_list->columns, new_columns);
    ast_create_query.columns_list->setOrReplace(ast_create_query.columns_list->indices, new_indices);
    ast_create_query.columns_list->setOrReplace(ast_create_query.columns_list->constraints, new_constraints);

    ASTStorage & storage_ast = *ast_create_query.storage;
    /// ORDER BY may change, but cannot appear, it's required construction
    if (metadata.order_by_ast && storage_ast.order_by)
        storage_ast.set(storage_ast.order_by, metadata.order_by_ast);

    if (metadata.primary_key_ast)
        storage_ast.set(storage_ast.primary_key, metadata.primary_key_ast);

    if (metadata.ttl_for_table_ast)
        storage_ast.set(storage_ast.ttl_table, metadata.ttl_for_table_ast);

    if (metadata.settings_ast)
        storage_ast.set(storage_ast.settings, metadata.settings_ast);


    statement = getObjectDefinitionFromCreateQuery(ast);
    {
        WriteBufferFromFile out(table_metadata_tmp_path, statement.size(), O_WRONLY | O_CREAT | O_EXCL);
        writeString(statement, out);
        out.next();
        if (context.getSettingsRef().fsync_metadata)
            out.sync();
        out.close();
    }

    try
    {
        /// rename atomically replaces the old file with the new one.
        Poco::File(table_metadata_tmp_path).renameTo(table_metadata_path);
    }
    catch (...)
    {
        Poco::File(table_metadata_tmp_path).remove();
        throw;
    }
}

}
