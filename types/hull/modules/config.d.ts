// hull:config - typed application configuration over the env capability.
declare module "hull:config" {
    /** The type a setting is coerced to. */
    export type ConfigType = "string" | "number" | "integer" | "boolean";
    /** The JavaScript type of a coerced setting. */
    export type ConfigValue<T extends ConfigType> = T extends "number" | "integer" ? number : T extends "boolean" ? boolean : string;
    /** Options for `config.get`. */
    export interface ConfigGetOptions<T extends ConfigType = "string", D = undefined> {
        /** Coerce to this type (default "string"); an uncoercible value throws. */
        type?: T;
        /** Returned (not coerced) when the setting is absent. */
        default?: D;
        /** Throw when the setting is absent. */
        required?: boolean;
    }
    /** The `hull:config` module object. A name must be in `manifest.env` (or a loaded `.env`) to be read. */
    export interface ConfigModule {
        /** A required setting: throws when absent. */
        get<T extends ConfigType = "string">(name: string, opts: ConfigGetOptions<T, unknown> & { required: true }): ConfigValue<T>;
        /** A setting, or `default` when absent. */
        get<T extends ConfigType = "string", D = undefined>(name: string, opts: ConfigGetOptions<T, D> & { default: D }): ConfigValue<T> | D;
        /** A setting, or undefined when absent. */
        get<T extends ConfigType = "string">(name: string, opts?: ConfigGetOptions<T, undefined>): ConfigValue<T> | undefined;
        /** A required setting (throws when absent). */
        require<T extends ConfigType = "string">(name: string, opts?: { type?: T }): ConfigValue<T>;
        /** Load a `.env` file (an fs.read path; default ".env"); only allowlisted keys apply. Returns how many applied. */
        loadDotenv(path?: string): number;
    }
    /** Typed configuration. */
    export const config: ConfigModule;
    /** Typed configuration (default export). */
    const _default: ConfigModule;
    export default _default;
}
