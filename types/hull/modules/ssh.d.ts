// hull:ssh - SSH client (exec, SFTP) over a manifest-granted connection. See docs/ssh.md.
declare module "hull:ssh" {
    /** Bytes in: a byte string (code units 0..255), an ArrayBuffer or a typed array. Bytes out are byte strings. */
    export type SshBytes = string | ArrayBuffer | ArrayBufferView;

    /** A private key loaded with `ssh.loadKey`. Only `algorithm` / `comment` / `blob` are meant for apps. */
    export interface SshPrivateKey {
        /** Key algorithm ("ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256", ...). */
        readonly algorithm: string;
        /** The key file's comment. */
        readonly comment: string;
        /** The public key blob (SSH wire encoding, byte string). */
        readonly blob: string;
        /** Signing material, varying by algorithm. */
        readonly [field: string]: unknown;
    }

    /** Options of `ssh.loadKey`. */
    export interface SshLoadKeyOptions {
        /** NAME of an environment variable holding the passphrase (read and scrubbed in C; preferred). */
        passphraseEnv?: string;
        /** The passphrase itself. */
        passphrase?: string;
    }

    /** A host-key trust store (app-owned); methods may be async. */
    export interface SshTrustStore {
        /** The stored key blob for a host name ("host" or "[host]:port"), or null. */
        get(name: string): string | null | undefined | Promise<string | null | undefined>;
        /** Store a key blob for a host name. */
        put(name: string, blob: string): void | Promise<void>;
        /** Forget a host name's key. */
        forget(name: string): void | Promise<void>;
        /** Whether a key blob is revoked. */
        revoked?(blob: string): boolean | Promise<boolean>;
        /** Every stored entry, name -> blob. */
        entries?(): { [name: string]: string };
    }

    /** A single-use trust from `ssh.pendingTrust()`: pins the first host key one connection presents. */
    export interface SshPendingTrust {
        /** Always true: marks the pending trust. */
        readonly pending: true;
        /** Whether a connect has taken it. */
        used: boolean;
        /** The pinned key blob, once a connection presented one. */
        key: string | null;
        /** The host name the key was pinned for. */
        name: string | null;
        /** The pinned key for `name`, or null. */
        get(name: string): string | null;
        /** The pinned entry, if any. */
        entries(): { [name: string]: string };
    }

    /** Options of `ssh.fileStore`. */
    export interface SshFileStoreOptions {
        /** Write new entries hashed (OpenSSH HashKnownHosts). */
        hash?: boolean;
    }

    /** The part of a hull:kv handle `ssh.kvStore` uses (its backend must support compare-and-swap). */
    export interface SshKvHandle {
        /** Read a key. */
        get(key: string): unknown;
        /** Compare-and-swap. */
        cas(key: string, expected: unknown, value: unknown): unknown;
        /** Delete a key. */
        delete(key: string): unknown;
        /** Iterate keys with a prefix. */
        scan(prefix: string): Iterable<string>;
        /** Backend capabilities. */
        readonly caps: { readonly compare_exchange?: boolean; readonly scan?: boolean };
    }

    /** Options of `ssh.kvStore`. */
    export interface SshKvStoreOptions {
        /** Key prefix (default "hostkey:"). */
        prefix?: string;
    }

    /** The algorithms a client offers (narrow or reorder the defaults; never widen). */
    export interface SshOffer {
        /** Key exchange methods. */
        kex: string[];
        /** Host key algorithms. */
        hostKey: string[];
        /** Ciphers. */
        cipher: string[];
        /** MACs. */
        mac: string[];
        /** Compression ("none"). */
        compression: string[];
    }

    /** Reaching the SSH host through a WebSocket relay (needs manifest `ssh.tunnel`). */
    export interface SshTunnelOptions {
        /** The relay's host name. */
        host: string;
        /** The relay's port (default 443). */
        port?: number;
        /** TLS to the relay (default true). */
        tls?: boolean;
        /** TLS server name, when it differs from `host`. */
        tlsHostname?: string;
        /** The upgrade request's path (starts with one "/"). */
        path?: string;
        /** Extra upgrade request headers, as "Name: value" lines. */
        headers?: string[];
        /** The destination header's name (default "Cf-Access-Jump-Destination"); false sends none. */
        destinationHeader?: string | false;
    }

    /** Options of `ssh.connect`; exactly one of `key` / `keys`. */
    export interface SshConnectOptions {
        /** The SSH host (granted by manifest `ssh.connect.hosts`). */
        host: string;
        /** The login user (granted by `ssh.connect.users`). */
        user: string;
        /** Port (default 22; an integer). */
        port?: number;
        /** One key: key-file text / bytes, or a loaded key. */
        key?: SshBytes | SshPrivateKey;
        /** Keys tried in order. */
        keys?: (SshBytes | SshPrivateKey)[];
        /** Passphrase for key-file text. */
        passphrase?: string;
        /** NAME of an environment variable holding the passphrase. */
        passphraseEnv?: string;
        /** Host-key trust (default: a fresh in-memory store, so an unknown host rejects `host_unknown`). */
        trust?: SshTrustStore | SshPendingTrust;
        /** Budget (ms, integer) to reach an authenticated connection (default 30000). */
        timeoutMs?: number;
        /** Silence (ms) before a keepalive (default 30000; 0 off). */
        keepaliveMs?: number;
        /** Unanswered keepalives before giving up (default 3). */
        keepaliveMax?: number;
        /** Longest silence (ms) from the server (default 60000; 0 off). */
        idleMs?: number;
        /** The algorithm offer. */
        offer?: SshOffer;
        /** Software version in the identification string. */
        software?: string;
        /** Called with server banners (pre-auth text); may be async. */
        onBanner?: (text: string) => void | Promise<void>;
        /** Reach the host through a WebSocket relay. */
        tunnel?: SshTunnelOptions;
    }

    /** A chunk source for exec stdin: returns the next chunk, or null / "" at the end. */
    export type SshStdinSource = () => SshBytes | null | undefined | Promise<SshBytes | null | undefined>;

    /** Options of `conn.exec`. */
    export interface SshExecOptions {
        /** Input: bytes (at most 128 KiB) or a chunk source. */
        stdin?: SshBytes | SshStdinSource;
        /** Stream stdout chunks instead of accumulating them. */
        onStdout?: (chunk: string) => void | Promise<void>;
        /** Stream stderr chunks instead of accumulating them. */
        onStderr?: (chunk: string) => void | Promise<void>;
        /** Most bytes accumulated per stream (default 8 MiB; a non-negative integer). */
        maxOutput?: number;
        /** Bound (ms) on the whole command. */
        timeoutMs?: number;
    }

    /** How a command killed by a signal ended. */
    export interface SshExitSignal {
        /** Signal name ("TERM", "KILL", ...). */
        signal: string;
        /** Whether it dumped core. */
        coreDumped: boolean;
        /** The server's message. */
        message: string;
    }

    /** The result of `conn.exec` (a non-zero status is an answer, not an error). */
    export interface SshExecResult {
        /** Remote exit status; null when killed by a signal. */
        status: number | null;
        /** Set when the command was killed by a signal. */
        signal: SshExitSignal | null;
        /** Accumulated stdout ("" when streamed). */
        stdout: string;
        /** Accumulated stderr ("" when streamed). */
        stderr: string;
    }

    /** The algorithms a connection negotiated. */
    export interface SshNegotiated {
        /** Key exchange. */
        kex: string;
        /** Host key algorithm. */
        hostKey: string;
        /** Cipher, client to server. */
        cipherC2s: string;
        /** Cipher, server to client. */
        cipherS2c: string;
        /** MAC, client to server (null for an AEAD cipher). */
        macC2s: string | null;
        /** MAC, server to client. */
        macS2c: string | null;
        /** Compression, client to server. */
        compressionC2s: string;
        /** Compression, server to client. */
        compressionS2c: string;
    }

    /** What a connection has moved. */
    export interface SshStats {
        /** Completed rekeys. */
        rekeys: number;
        /** 0-based index into `keys` of the key that authenticated. */
        authKey: number;
        /** Bytes sent. */
        bytesSent: number;
        /** Bytes received. */
        bytesReceived: number;
        /** Packets sent. */
        packetsSent: number;
        /** Packets received. */
        packetsReceived: number;
        /** Whether the current keys are due for a rekey. */
        rekeyDue: boolean;
    }

    /** The host key a connection is bound to. */
    export interface SshHostKey {
        /** The key blob (SSH wire encoding, byte string). */
        blob: string;
        /** The key type it declares. */
        type: string;
        /** The negotiated signature algorithm. */
        algorithm: string | undefined;
        /** SHA256:<base64> fingerprint. */
        fingerprint: string;
    }

    /** SFTP file attributes (a field the server did not send is absent). */
    export interface SftpAttrs {
        /** Size in bytes (null past 2^53 - 1). */
        size?: number | null;
        /** Owner uid. */
        uid?: number;
        /** Owner gid. */
        gid?: number;
        /** Mode bits incl. file type. */
        permissions?: number;
        /** Access time (Unix seconds). */
        atime?: number;
        /** Modification time (Unix seconds). */
        mtime?: number;
        /** Whether it is a directory (from `permissions`). */
        isDir?: boolean;
    }

    /** Attributes `setstat` sets: any of size, uid + gid, atime + mtime, permissions. */
    export interface SftpSetAttrs {
        /** New size. */
        size?: number;
        /** New owner uid (with gid). */
        uid?: number;
        /** New owner gid (with uid). */
        gid?: number;
        /** New mode bits. */
        permissions?: number;
        /** Access time (with mtime). */
        atime?: number;
        /** Modification time (with atime). */
        mtime?: number;
    }

    /** One directory entry. */
    export interface SftpEntry {
        /** The exact file name (byte string). */
        filename: string;
        /** The server's `ls -l` line, cleaned for display. */
        longname: string;
        /** Its attributes. */
        attrs: SftpAttrs;
    }

    /** A directory listing. */
    export interface SftpListing {
        /** Entries whose names are safe to use as local path components. */
        entries: SftpEntry[];
        /** Entries refused, and why. */
        refused: { filename: string; reason: string }[];
    }

    /** A permission mode: an octal string ("755") or a number (0o755). */
    export type SftpMode = string | number;

    /** An open remote file. */
    export interface SftpFile {
        /** Read up to `n` bytes (default 32 KiB); "" at end of file. */
        read(n?: number): Promise<string>;
        /** Write bytes at the position -> true. */
        write(data: SshBytes): Promise<true>;
        /** Set the position (a non-negative integer) -> true. */
        seek(offset: number): true;
        /** The current position. */
        tell(): number;
        /** The file's attributes. */
        stat(): Promise<SftpAttrs>;
        /** Close the file -> true. */
        close(): Promise<true>;
    }

    /** Options of `sftp.open`. */
    export interface SftpOpenOptions {
        /** A new file's permissions. */
        mode?: SftpMode;
    }

    /** An SFTP session; every failure rejects with an `SshError`. */
    export interface SftpSession {
        /** The canonical absolute form of a path. */
        realpath(path: SshBytes): Promise<string | null>;
        /** List a directory. */
        list(path: SshBytes): Promise<SftpListing>;
        /** Read a whole file (at most `max` bytes, default 16 MiB). */
        read(path: SshBytes, max?: number): Promise<string>;
        /** Write a whole file, creating or truncating it -> true. */
        write(path: SshBytes, data: SshBytes): Promise<true>;
        /** Attributes, following a symlink. */
        stat(path: SshBytes): Promise<SftpAttrs>;
        /** Attributes, not following a symlink. */
        lstat(path: SshBytes): Promise<SftpAttrs>;
        /** Create a directory -> true. */
        mkdir(path: SshBytes, mode?: SftpMode): Promise<true>;
        /** Remove an empty directory -> true. */
        rmdir(path: SshBytes): Promise<true>;
        /** Remove a file -> true. */
        remove(path: SshBytes): Promise<true>;
        /** Rename (refused when `to` exists) -> true. */
        rename(from: SshBytes, to: SshBytes): Promise<true>;
        /** Set permissions -> true. */
        chmod(path: SshBytes, mode: SftpMode): Promise<true>;
        /** Set attributes -> true. */
        setstat(path: SshBytes, attrs: SftpSetAttrs): Promise<true>;
        /** Open a file: "r", "r+", "w" (create / truncate), "a" (append), "wx" (create, must not exist). */
        open(path: SshBytes, mode?: "r" | "r+" | "w" | "a" | "wx", opts?: SftpOpenOptions): Promise<SftpFile>;
        /** Close the session. */
        close(): Promise<void>;
    }

    /** Options of `conn.sftp`. */
    export interface SftpOptions {
        /** Wait (ms) for the server's next message (default 60000; 0 none). */
        replyTimeoutMs?: number;
    }

    /** An authenticated SSH connection. */
    export interface SshConnection {
        /** Run one command (no PTY). */
        exec(command: SshBytes, opts?: SshExecOptions): Promise<SshExecResult>;
        /** Open an SFTP session. */
        sftp(opts?: SftpOptions): Promise<SftpSession>;
        /** Close the connection. */
        close(): void;
        /** The host key's SHA256 fingerprint. */
        fingerprint(): string;
        /** The negotiated algorithms. */
        negotiated(): SshNegotiated;
        /** Whether the connection was made with `ssh.pendingTrust()`. */
        trustPending(): boolean;
        /** The host key the connection is bound to. */
        hostKey(): SshHostKey;
        /** The RFC 4253 session identifier (byte string). */
        sessionId(): string;
        /** Record this connection's host key in a real store -> true. */
        acceptHost(store: SshTrustStore): Promise<true>;
        /** Ask the server for new keys now -> true. */
        rekey(): Promise<true>;
        /** What the connection has moved. */
        stats(): SshStats;
    }

    /** The coded Error every ssh failure rejects (or throws) with; argument misuse is a TypeError. */
    export interface SshError extends Error {
        /** Stable code: host_unknown, host_changed, host_revoked, auth_failed, timeout, connect_failed, no_such_file, ... */
        code: string;
        /** Human-readable detail. */
        detail?: string;
        /** The presented host key's fingerprint (host_unknown / host_changed). */
        fingerprint?: string;
        /** The stored key's fingerprint (host_changed). */
        storedFingerprint?: string;
        /** The presented host key blob (host_unknown / host_changed). */
        keyBlob?: string;
        /** Auth methods the server offered (auth_failed / partial_success). */
        methods?: string[];
        /** 0-based index of the key in `keys` that failed to load (bad_key). */
        key?: number;
        /** The socket error's name (connect_failed), e.g. "EHOSTUNREACH". */
        errno?: string;
        /** The limit passed (too_large). */
        limit?: number;
    }

    /** The `hull:ssh` module object. */
    export interface SshModule {
        /** Load a key from key-file contents; throws an `SshError` (bad_key, bad_passphrase, ...). */
        loadKey(text: SshBytes, opts?: SshLoadKeyOptions): SshPrivateKey;
        /** A single-use trust for one connection (enrollment). */
        pendingTrust(): SshPendingTrust;
        /** An in-memory trust store, optionally seeded with name -> blob. */
        memoryStore(seed?: { [name: string]: string }): SshTrustStore & { entries(): { [name: string]: string } };
        /** A trust store in an OpenSSH known_hosts file (through hull:fs). */
        fileStore(path: string, opts?: SshFileStoreOptions): SshTrustStore & { revoked(blob: string): boolean; entries(): { [name: string]: string } };
        /** A trust store in a hull:kv namespace whose backend supports compare-and-swap. */
        kvStore(kv: SshKvHandle, opts?: SshKvStoreOptions): SshTrustStore & { entries(): { [name: string]: string } };
        /** Open an authenticated connection; rejects with an `SshError`. */
        connect(opts: SshConnectOptions): Promise<SshConnection>;
        /** Trust a host key for host (and port, default 22) -> true. */
        acceptHost(trust: SshTrustStore, host: string, keyBlob: SshBytes, port?: number): Promise<true>;
        /** Forget the key stored for host (and port, default 22) -> true. */
        forgetHost(trust: SshTrustStore, host: string, port?: number): Promise<true>;
        /** A key blob's fingerprint: "SHA256:<base64>". */
        fingerprint(keyBlob: SshBytes): string;
        /** Default `timeoutMs` of `connect` (30000). */
        readonly CONNECT_TIMEOUT_MS: number;
    }
    /** The SSH client. */
    export const ssh: SshModule;
    /** The SSH client (default export). */
    export default ssh;
}
