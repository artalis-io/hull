// hull:search - full-text search over SQLite FTS5 (the default connection; SQLite only).
declare module "hull:search" {
    /** Options for `search.createIndex`. */
    export interface SearchIndexOptions {
        /** FTS5 tokenizer spec (default "unicode61"). */
        tokenize?: string;
        /** External content table name. */
        content?: string;
        /** Rowid column of the external content table. */
        contentRowid?: string;
    }

    /** `opts.snippet` for `search.query`: the indexed text is HTML-escaped, the markers inserted as given. */
    export interface SearchSnippetOptions {
        /** Column index (0 is the id; default 1, the first indexed column). */
        column?: number;
        /** Tokens in the snippet (1..64, default 32). */
        tokens?: number;
        /** Marker before a match (default "<b>"). */
        before?: string;
        /** Marker after a match (default "</b>"). */
        after?: string;
        /** Marker for elided text (default "..."). */
        ellipsis?: string;
    }

    /** `opts.highlight` for `search.query`. */
    export interface SearchHighlightOptions {
        /** Column index (0 is the id; default 1). */
        column?: number;
        /** Marker before a match (default "<b>"). */
        before?: string;
        /** Marker after a match (default "</b>"). */
        after?: string;
    }

    /** Options for `search.query`. */
    export interface SearchQueryOptions {
        /** Rows to return (default 20). */
        limit?: number;
        /** Rows to skip (default 0). */
        offset?: number;
        /** Add a `snippet` field to each hit. */
        snippet?: SearchSnippetOptions;
        /** Add a `highlight` field to each hit. */
        highlight?: SearchHighlightOptions;
        /** Order by FTS5 rank (default) or rowid. */
        order?: "rank" | "rowid";
        /** Longest query accepted, in UTF-8 bytes (default 1024). */
        maxQueryLen?: number;
        /** Most terms / operators / groups accepted (default 64). */
        maxQueryTerms?: number;
    }

    /** One search hit. */
    export interface SearchHit {
        /** The document id it was indexed under. */
        id: string | number;
        /** FTS5 rank (lower is better). */
        rank: number;
        /** With `opts.snippet`: the escaped snippet. */
        snippet?: string;
        /** With `opts.highlight`: the escaped, highlighted column. */
        highlight?: string;
    }

    /** The hits; a query FTS5 cannot parse returns none, with `error` set. */
    export type SearchResult = SearchHit[] & { error?: string };

    /** Options for `search.reindex`. */
    export interface SearchReindexOptions {
        /** FTS column -> source column. */
        columns: { [ftsColumn: string]: string };
        /** The source's id column (default "id"). */
        idColumn?: string;
    }

    /** The `hull:search` module object. */
    export interface SearchModule {
        /** Create an FTS5 index (idempotent). */
        createIndex(name: string, columns: readonly string[], opts?: SearchIndexOptions): void;
        /** Drop an index (idempotent). */
        dropIndex(name: string): void;
        /** Insert or replace a document; field values are strings or numbers (null / undefined fields are skipped). */
        index(name: string, id: string | number, fields: { [column: string]: string | number | null | undefined }): void;
        /** Remove a document. */
        remove(name: string, id: string | number): void;
        /** Run an FTS5 MATCH query. */
        query(name: string, q: string, opts?: SearchQueryOptions): SearchResult;
        /** Rebuild an index from a source table, in one transaction. */
        reindex(name: string, sourceTable: string, opts: SearchReindexOptions): void;
    }
    /** Full-text search. */
    export const search: SearchModule;
}
