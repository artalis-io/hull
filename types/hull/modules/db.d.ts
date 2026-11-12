// hull:db - database connections (SQLite, and the composed Postgres / MySQL / DuckDB backends).
// Fills the shared Hull.DbConnection by declaration merging.
declare namespace Hull {
    /** A database connection (`db.default()`, `db.connect(name)`, `db.open(dsn)`). Sealed: its members cannot be replaced. */
    interface DbConnection {
        /** Run a query; returns its rows. Throws on a SQL error or a reserved `_hull_*` table. */
        query<R extends object = Hull.DataRow>(sql: string, params?: readonly import("hull:db").DbParam[]): R[];
        /** Run a statement; returns the number of rows it changed. */
        exec(sql: string, params?: readonly import("hull:db").DbParam[]): number;
        /** The row id of the last INSERT on this connection (read it right after the INSERT, with no await between). */
        lastId(): number;
        /** Run `fn` inside a transaction (a savepoint when nested); commits when it returns, rolls back when it throws. `fn` must be synchronous. */
        batch<R>(fn: () => R & ([R] extends [PromiseLike<unknown>] ? "db.batch: fn must be synchronous" : unknown)): void;
        /** INSERT a row unless one with the same `conflictCols` exists; returns the rows inserted (0 or 1). */
        insertIfAbsent(table: string, conflictCols: readonly string[], cols: readonly string[], values: readonly import("hull:db").DbParam[]): number;
        /** INSERT a row, or UPDATE the one with the same `conflictCols`; returns the rows changed. */
        upsert(table: string, conflictCols: readonly string[], cols: readonly string[], values: readonly import("hull:db").DbParam[]): number;
        /** The column names of a table (empty when it does not exist). */
        tableColumns(table: string): string[];
        /** `name` quoted as an identifier for this backend (internal quotes doubled). */
        quoteIdentifier(name: string): string;
        /** Whether the connection is inside a transaction. */
        inTransaction(): boolean;
        /** Wait for a NOTIFY on `channel` (Postgres): resolves true when notified, false on timeout. Throws where `dialect.supportsNotify` is false. */
        waitNotify(channel: string, timeoutMs?: number): Promise<boolean>;
        /** Query / exec on the worker pool (Promise-returning). */
        readonly async: import("hull:db").DbAsync;
        /** User-defined SQL functions; present only on a SQLite connection (`conn.udf !== undefined` is the capability probe). */
        readonly udf?: import("hull:db").DbUdf;
        /** The backend's name: `"sqlite"`, `"postgres"`, `"mysql"`, `"duckdb"`. */
        readonly backendName: "sqlite" | "postgres" | "mysql" | "duckdb";
        /** The dialect's auto-increment id column DDL (an escape hatch; prefer migrations). */
        readonly autoincrementIdDdl: string;
        /** The backend's SQL dialect descriptor. */
        readonly dialect: import("hull:db").DbDialect;
    }

}

declare module "hull:db" {
    /** A value a column holds in a query row: integers and reals are numbers, BLOBs ArrayBuffers, Postgres booleans booleans, NULL null. */
    export type DbValue = number | string | boolean | ArrayBuffer | null;
    /** A bound parameter: a string, number, boolean, BigInt in the int64 range, null / undefined (NULL), or bytes (a BLOB). Anything else throws a TypeError. */
    export type DbParam = string | number | boolean | bigint | null | undefined | Hull.Bytes;
    /** A query row as the backend returns it: column name -> value (what `DbValue` lists). `conn.query` rows default to `Hull.DataRow`. */
    export type DbRow = { [column: string]: DbValue };

    /** What `conn.async.exec` resolves to. */
    export interface DbExecResult {
        /** Rows the statement changed. */
        changes: number;
        /** The last inserted row id on the worker connection. */
        lastId: number;
    }

    /** `conn.async`: the same query / exec run on the worker pool's own connection to this database. */
    export interface DbAsync {
        /** Run a query on a worker connection; resolves to its rows. */
        query<R extends object = Hull.DataRow>(sql: string, params?: readonly DbParam[]): Promise<R[]>;
        /** Run a statement on a worker connection; resolves to `{ changes, lastId }`. */
        exec(sql: string, params?: readonly DbParam[]): Promise<DbExecResult>;
    }

    /** A JavaScript scalar UDF: called once per row with the SQL arguments; returns the SQL result. */
    export type DbUdfScalar = (...args: Hull.Data[]) => DbParam;
    /** A JavaScript aggregate UDF: `step(state, ...args)` per row, `finalize(state)` returns the result. `state` is a fresh object per group. */
    export interface DbUdfAggregate {
        /** Called for each row of the group. */
        step(state: { [key: string]: Hull.Data }, ...args: Hull.Data[]): void;
        /** Called once per group; its return value is the aggregate's result. */
        finalize(state: { [key: string]: Hull.Data }): DbParam;
    }
    /** Options for `conn.udf.register`. */
    export interface DbUdfOptions {
        /** Lets SQLite's optimizer treat the function as deterministic (default false). */
        deterministic?: boolean;
        /** Number of SQL arguments; -1 = variadic (default 1 for a WASM UDF). */
        args?: number;
        /** WASM UDFs: aggregate mode (default false). */
        aggregate?: boolean;
        /** WASM UDFs: per-row gas limit (default 100000). */
        gas?: number;
        /** WASM UDFs: linear-memory heap size in bytes. */
        heap?: number;
        /** WASM UDFs: stack size in bytes. */
        stack?: number;
    }
    /** `conn.udf`: user-defined SQL functions (SQLite connections only). */
    export interface DbUdf {
        /** Register a UDF: a JS function, a `{ step, finalize }` aggregate, or the name of a WASM compute module. Names must start with `hull_`. */
        register(name: `hull_${string}`, impl: DbUdfScalar | DbUdfAggregate | string, opts?: DbUdfOptions): void;
        /** Remove a registered UDF. */
        unregister(name: string): void;
    }

    /** The backend's SQL dialect descriptor (read-only). */
    export interface DbDialect {
        /** The identifier quote character (`"`, or a backtick on MySQL). */
        readonly identifierQuote: string;
        /** The placeholder the backend's SQL uses (`?` or `$`). */
        readonly placeholder: "?" | "$n";
        /** How the backend spells an upsert (`on_conflict`, `on_duplicate_key`, ...). */
        readonly upsertStyle: "on_conflict" | "on_duplicate_key";
        /** Whether `RETURNING` is supported. */
        readonly supportsReturning: boolean;
        /** Whether `CREATE INDEX IF NOT EXISTS` is supported natively. */
        readonly supportsIndexIfNotExists: boolean;
        /** Whether `FOR UPDATE SKIP LOCKED` is supported. */
        readonly supportsSkipLocked: boolean;
        /** Whether LISTEN / NOTIFY (`conn.waitNotify`) is supported. */
        readonly supportsNotify: boolean;
        /** The DDL fragment for an auto-increment id column. */
        readonly identityColumn: string;
        /** DuckDB only: a template naming the identity sequence. */
        readonly identitySequence?: string;
    }

    /** A caller-owned connection from `db.open(dsn)`: close it, or let GC finalize it. */
    export interface DbOwnedConnection extends Hull.DbConnection {
        /** Close the connection (idempotent; later calls on it throw). */
        close(): void;
    }
    /** The `hull:db` module object: connection acquisition only - queries go through a connection. */
    export interface DbModule {
        /** The `-d` / `"default"` connection. */
        default(): Hull.DbConnection;
        /** A named connection from `manifest.databases.named` (call after load: from app.main or a handler). Throws for an unknown name. */
        connect(name: string): Hull.DbConnection;
        /** Open a caller-owned connection to a DSN allowed by `manifest.databases.dynamic`. */
        open(dsn: string): DbOwnedConnection;
    }
    /** Database connections. */
    export const db: DbModule;
}
