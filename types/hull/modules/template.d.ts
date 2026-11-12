// hull:template - server-side templates from templates/ (HTML-escaped by default).
declare module "hull:template" {
    /** The data a template renders: its top-level names. */
    export type TemplateData = object;

    /** A compiled template. */
    export type TemplateRender = (data?: TemplateData | null) => string;

    /** The template engine. */
    export const template: {
        /** Render `templates/<name>` with `data` (compiled once, cached). */
        readonly render: (name: string, data?: TemplateData | null) => string;
        /** Compile and render a source string (never cached). */
        readonly renderString: (source: string, data?: TemplateData | null) => string;
        /** The cached render function of `templates/<name>`; throws when missing or malformed. */
        readonly compile: (name: string) => TemplateRender;
        /** Clear the compiled-template cache. */
        readonly clearCache: () => void;
    };
}
