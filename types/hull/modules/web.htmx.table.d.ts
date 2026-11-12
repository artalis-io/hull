// hull:web:htmx:table - schema-driven sortable, inline-editable <table>.
declare module "hull:web:htmx:table" {
    import type { SortState } from "hull:web:htmx:sort";
    /** A table row: column name -> value. */
    export type TableRow = { [column: string]: unknown };
    /** One column of the table schema. */
    export interface TableColumn<R extends TableRow = TableRow> {
        /** The row key this column shows. */
        name: string;
        /** Header text (default: the name). */
        label?: string;
        /** Render a sortable header. */
        sortable?: boolean;
        /** Render the cell as an inline-edit cell (needs `opts.editUrlFor`). */
        editable?: boolean;
        /** Custom cell renderer; a string return is spliced RAW (escape it yourself), anything else is escaped. */
        render?: (value: unknown, row: R) => unknown;
    }
    /** Options for `table.render`. */
    export interface TableRenderOptions<R extends TableRow = TableRow> {
        /** Base URL for the sort links (required). */
        baseUrl: string;
        /** `hx-target` for sort swaps (default "closest table"). */
        target?: string;
        /** `hx-swap` for sort swaps. */
        swap?: string;
        /** The current sort (from `sort.parse`). */
        currentSort?: SortState | null;
        /** Push the sort URL (default true). */
        pushUrl?: boolean;
        /** Edit URL of an editable cell; required when a column is editable. */
        editUrlFor?: (row: R, column: string) => string;
        /** Text of the empty-table row (default "No rows."). */
        emptyLabel?: string;
    }
    /** The `hull:web:htmx:table` module object. */
    export interface TableModule {
        /** Full `<table>...</table>` HTML. Throws without `opts.baseUrl`. */
        render<R extends TableRow>(rows: R[] | null | undefined, schema: TableColumn<R>[], opts: TableRenderOptions<R>): string;
    }
    /** htmx table widget. */
    export const table: TableModule;
    export default table;
}
