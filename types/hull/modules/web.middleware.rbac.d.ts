// hull:web:middleware:rbac - role-based access control.
declare module "hull:web:middleware:rbac" {
    /** A user id: a non-empty string or an integer. */
    export type RbacUserId = string | number;
    /** Options for `requireRole` / `requirePermission`. */
    export interface RbacGuardOptions {
        /** The request's user id (default `req.ctx.session.user_id`); no id answers 401. */
        getUserId?: (req: Hull.Request) => RbacUserId | null | undefined;
    }
    /** The `rbac` module object. */
    export interface RbacModule {
        /** Create the four `_hull_*` RBAC tables (idempotent). */
        init(opts?: Record<string, never>): void;
        /** Define a role, granting it the given permission(s) (idempotent). */
        defineRole(name: string, permissions?: string | string[]): void;
        /** Define a permission (idempotent). */
        definePermission(name: string): void;
        /** Assign a role to a user (idempotent). */
        assign(userId: RbacUserId, role: string): void;
        /** Revoke a role from a user. */
        revoke(userId: RbacUserId, role: string): void;
        /** Grant a permission to a role (idempotent). */
        grant(role: string, permission: string): void;
        /** Remove a permission from a role. */
        ungrant(role: string, permission: string): void;
        /** The user's roles. */
        roles(userId: RbacUserId): string[];
        /** The user's permissions, through any role. */
        permissions(userId: RbacUserId): string[];
        /** Whether the user has the role. */
        hasRole(userId: RbacUserId, role: string): boolean;
        /** Whether the user has the permission through any role. */
        hasPermission(userId: RbacUserId, permission: string): boolean;
        /** Whether the user has any of the roles. */
        hasAnyRole(userId: RbacUserId, roleList: readonly string[]): boolean;
        /** Whether the user has any of the permissions. */
        hasAnyPermission(userId: RbacUserId, permList: readonly string[]): boolean;
        /** Middleware: 401 without a user, 403 unless the user holds the role (or any listed). */
        requireRole(roleOrRoles: string | readonly string[], opts?: RbacGuardOptions): Hull.Middleware;
        /** Middleware: 401 without a user, 403 unless the user holds the permission (or any listed). */
        requirePermission(permOrPerms: string | readonly string[], opts?: RbacGuardOptions): Hull.Middleware;
    }
    /** Role-based access control. */
    export const rbac: RbacModule;
}
