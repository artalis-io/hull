// hull:uuid - RFC 9562 UUIDs.
declare module "hull:uuid" {
    /** The UUID generators. */
    export interface Uuid {
        /** A random (version 4) UUID, 36-char canonical form. */
        readonly v4: () => string;
        /** A time-ordered (version 7) UUID, lexically sortable by creation time. */
        readonly v7: () => string;
    }

    /** UUID generation. */
    export const uuid: Uuid;
    export default uuid;
}
