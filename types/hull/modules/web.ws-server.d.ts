// hull:web:ws-server - WebSocket server fan-out. Declaring `hull/web/ws-server`
// also installs app.ws (see hull:app); the connection shape is Hull.WsConnection.
declare module "hull:web:ws-server" {
    /** WebSocket server helpers. */
    export const wsServer: {
        /** Send `data` to every connection on `path` (binary frames when `binary`); returns how many it reached. */
        broadcast(path: string, data: string | Hull.Bytes, binary?: boolean): number;
        /** How many connections are open on `path`. */
        connections(path: string): number;
    };
}
