#include <Interpreters/ClusterProxy/SelectStreamFactory.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <DataStreams/RemoteBlockInputStream.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <DataStreams/LazyBlockInputStream.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/VirtualColumnUtils.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/checkStackSize.h>
#include <TableFunctions/TableFunctionFactory.h>

#include <common/logger_useful.h>
#include <DataStreams/ConvertingBlockInputStream.h>
#include <Processors/Pipe.h>
#include <Processors/Transforms/ConvertingTransform.h>
#include <Processors/Sources/SourceFromInputStream.h>
#include <Processors/Executors/TreeExecutorBlockInputStream.h>

namespace ProfileEvents
{
    extern const Event DistributedConnectionMissingTable;
    extern const Event DistributedConnectionStaleReplica;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int ALL_CONNECTION_TRIES_FAILED;
    extern const int ALL_REPLICAS_ARE_STALE;
}

namespace ClusterProxy
{

SelectStreamFactory::SelectStreamFactory(
    const Block & header_,
    QueryProcessingStage::Enum processed_stage_,
    QualifiedTableName main_table_,
    const Scalars & scalars_,
    bool has_virtual_shard_num_column_,
    const Tables & external_tables_)
    : header(header_),
    processed_stage{processed_stage_},
    main_table(std::move(main_table_)),
    table_func_ptr{nullptr},
    scalars{scalars_},
    has_virtual_shard_num_column(has_virtual_shard_num_column_),
    external_tables{external_tables_}
{
}

SelectStreamFactory::SelectStreamFactory(
    const Block & header_,
    QueryProcessingStage::Enum processed_stage_,
    ASTPtr table_func_ptr_,
    const Scalars & scalars_,
    bool has_virtual_shard_num_column_,
    const Tables & external_tables_)
    : header(header_),
    processed_stage{processed_stage_},
    table_func_ptr{table_func_ptr_},
    scalars{scalars_},
    has_virtual_shard_num_column(has_virtual_shard_num_column_),
    external_tables{external_tables_}
{
}

namespace
{

Pipe createLocalStream(const ASTPtr & query_ast, const Block & header, const Context & context, QueryProcessingStage::Enum processed_stage, bool force_tree_shaped_pipeline)
{
    checkStackSize();

    InterpreterSelectQuery interpreter{query_ast, context, SelectQueryOptions(processed_stage)};

    if (force_tree_shaped_pipeline)
    {
        /// This flag means that pipeline must be tree-shaped,
        /// so we can't enable processors for InterpreterSelectQuery here.
        auto stream = interpreter.execute().in;
        Pipe pipe(std::make_shared<SourceFromInputStream>(std::move(stream)));

        pipe.addSimpleTransform(std::make_shared<ConvertingTransform>(
                pipe.getHeader(), header, ConvertingTransform::MatchColumnsMode::Name, context));

        return pipe;
    }

    auto pipeline = interpreter.executeWithProcessors();

    pipeline.addSimpleTransform([&](const Block & source_header)
    {
        return std::make_shared<ConvertingTransform>(
                source_header, header, ConvertingTransform::MatchColumnsMode::Name, context);
    });

    /** Materialization is needed, since from remote servers the constants come materialized.
      * If you do not do this, different types (Const and non-Const) columns will be produced in different threads,
      * And this is not allowed, since all code is based on the assumption that in the block stream all types are the same.
      */

    /* Now we don't need to materialize constants, because RemoteBlockInputStream will ignore constant and take it from header.
     * So, streams from different threads will always have the same header.
     */
    /// return std::make_shared<MaterializingBlockInputStream>(stream);

    return std::move(pipeline).getPipe();
}

static String formattedAST(const ASTPtr & ast)
{
    if (!ast)
        return "";
    std::stringstream ss;
    formatAST(*ast, ss, false, true);
    return ss.str();
}

}

void SelectStreamFactory::createForShard(
    const Cluster::ShardInfo & shard_info,
    const String &, const ASTPtr & query_ast,
    const Context & context, const ThrottlerPtr & throttler,
    const SelectQueryInfo & query_info,
    Pipes & res)
{
    bool force_add_agg_info = processed_stage == QueryProcessingStage::WithMergeableState;
    bool add_totals_port = processed_stage == QueryProcessingStage::Complete;

    auto modified_query_ast = query_ast->clone();
    if (has_virtual_shard_num_column)
        VirtualColumnUtils::rewriteEntityInAst(modified_query_ast, "_shard_num", shard_info.shard_num, "toUInt32");

    auto emplace_local_stream = [&]()
    {
        res.emplace_back(createLocalStream(modified_query_ast, header, context, processed_stage, query_info.force_tree_shaped_pipeline));
    };

    String modified_query = formattedAST(modified_query_ast);

    auto emplace_remote_stream = [&]()
    {
        auto stream = std::make_shared<RemoteBlockInputStream>(
            shard_info.pool, modified_query, header, context, nullptr, throttler, scalars, external_tables, processed_stage);
        stream->setPoolMode(PoolMode::GET_MANY);
        if (!table_func_ptr)
            stream->setMainTable(main_table);

        auto source = std::make_shared<SourceFromInputStream>(std::move(stream), force_add_agg_info);

        if (add_totals_port)
            source->addTotalsPort();

        res.emplace_back(std::move(source));
    };

    const auto & settings = context.getSettingsRef();

    if (settings.prefer_localhost_replica && shard_info.isLocal())
    {
        StoragePtr main_table_storage;

        if (table_func_ptr)
        {
            const auto * table_function = table_func_ptr->as<ASTFunction>();
            TableFunctionPtr table_function_ptr = TableFunctionFactory::instance().get(table_function->name, context);
            main_table_storage = table_function_ptr->execute(table_func_ptr, context, table_function_ptr->getName());
        }
        else
            main_table_storage = context.tryGetTable(main_table.database, main_table.table);


        if (!main_table_storage) /// Table is absent on a local server.
        {
            ProfileEvents::increment(ProfileEvents::DistributedConnectionMissingTable);
            if (shard_info.hasRemoteConnections())
            {
                LOG_WARNING(
                        &Logger::get("ClusterProxy::SelectStreamFactory"),
                        "There is no table " << main_table.database << "." << main_table.table
                        << " on local replica of shard " << shard_info.shard_num << ", will try remote replicas.");
                emplace_remote_stream();
            }
            else
                emplace_local_stream();  /// Let it fail the usual way.

            return;
        }

        const auto * replicated_storage = dynamic_cast<const StorageReplicatedMergeTree *>(main_table_storage.get());

        if (!replicated_storage)
        {
            /// Table is not replicated, use local server.
            emplace_local_stream();
            return;
        }

        UInt64 max_allowed_delay = settings.max_replica_delay_for_distributed_queries;

        if (!max_allowed_delay)
        {
            emplace_local_stream();
            return;
        }

        UInt32 local_delay = replicated_storage->getAbsoluteDelay();

        if (local_delay < max_allowed_delay)
        {
            emplace_local_stream();
            return;
        }

        /// If we reached this point, local replica is stale.
        ProfileEvents::increment(ProfileEvents::DistributedConnectionStaleReplica);
        LOG_WARNING(
            &Logger::get("ClusterProxy::SelectStreamFactory"),
            "Local replica of shard " << shard_info.shard_num << " is stale (delay: " << local_delay << "s.)");

        if (!settings.fallback_to_stale_replicas_for_distributed_queries)
        {
            if (shard_info.hasRemoteConnections())
            {
                /// If we cannot fallback, then we cannot use local replica. Try our luck with remote replicas.
                emplace_remote_stream();
                return;
            }
            else
                throw Exception(
                    "Local replica of shard " + toString(shard_info.shard_num)
                    + " is stale (delay: " + toString(local_delay) + "s.), but no other replica configured",
                    ErrorCodes::ALL_REPLICAS_ARE_STALE);
        }

        if (!shard_info.hasRemoteConnections())
        {
            /// There are no remote replicas but we are allowed to fall back to stale local replica.
            emplace_local_stream();
            return;
        }

        /// Try our luck with remote replicas, but if they are stale too, then fallback to local replica.
        /// Do it lazily to avoid connecting in the main thread.

        auto lazily_create_stream = [
                pool = shard_info.pool, shard_num = shard_info.shard_num, modified_query, header = header, modified_query_ast, context, throttler,
                main_table = main_table, table_func_ptr = table_func_ptr, scalars = scalars, external_tables = external_tables,
                stage = processed_stage, local_delay]()
            -> BlockInputStreamPtr
        {
            auto current_settings = context.getSettingsRef();
            auto timeouts = ConnectionTimeouts::getTCPTimeoutsWithFailover(
                current_settings).getSaturated(
                    current_settings.max_execution_time);
            std::vector<ConnectionPoolWithFailover::TryResult> try_results;
            try
            {
                if (table_func_ptr)
                    try_results = pool->getManyForTableFunction(timeouts, &current_settings, PoolMode::GET_MANY);
                else
                    try_results = pool->getManyChecked(timeouts, &current_settings, PoolMode::GET_MANY, main_table);
            }
            catch (const Exception & ex)
            {
                if (ex.code() == ErrorCodes::ALL_CONNECTION_TRIES_FAILED)
                    LOG_WARNING(
                        &Logger::get("ClusterProxy::SelectStreamFactory"),
                        "Connections to remote replicas of local shard " << shard_num << " failed, will use stale local replica");
                else
                    throw;
            }

            double max_remote_delay = 0.0;
            for (const auto & try_result : try_results)
            {
                if (!try_result.is_up_to_date)
                    max_remote_delay = std::max(try_result.staleness, max_remote_delay);
            }

            if (try_results.empty() || local_delay < max_remote_delay)
                return std::make_shared<TreeExecutorBlockInputStream>(createLocalStream(modified_query_ast, header, context, stage, true));
            else
            {
                std::vector<IConnectionPool::Entry> connections;
                connections.reserve(try_results.size());
                for (auto & try_result : try_results)
                    connections.emplace_back(std::move(try_result.entry));

                return std::make_shared<RemoteBlockInputStream>(
                    std::move(connections), modified_query, header, context, nullptr, throttler, scalars, external_tables, stage);
            }
        };

        auto lazy_stream = std::make_shared<LazyBlockInputStream>("LazyShardWithLocalReplica", header, lazily_create_stream);
        auto source = std::make_shared<SourceFromInputStream>(std::move(lazy_stream), force_add_agg_info);

        if (add_totals_port)
            source->addTotalsPort();

        res.emplace_back(std::move(source));
    }
    else
        emplace_remote_stream();
}

}
}
