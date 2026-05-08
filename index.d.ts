import { EventEmitter } from "events";

type ConnectOptions = {
	share_mode?: number;
	protocol?: number;
};

type Status = {
	atr?: Buffer;
	state: number;
};

interface PCSCLite extends EventEmitter {
	on(type: "error", listener: (error: any) => void): this;

	once(type: "error", listener: (error: any) => void): this;

	on(type: "reader", listener: (reader: CardReader) => void): this;

	once(type: "reader", listener: (reader: CardReader) => void): this;

	close(): void;
}

interface CardReader extends EventEmitter {
	// Share Mode
	SCARD_SHARE_SHARED: number;
	SCARD_SHARE_EXCLUSIVE: number;
	SCARD_SHARE_DIRECT: number;
	// Protocol
	SCARD_PROTOCOL_T0: number;
	SCARD_PROTOCOL_T1: number;
	SCARD_PROTOCOL_RAW: number;
	//  State
	SCARD_STATE_UNAWARE: number;
	SCARD_STATE_IGNORE: number;
	SCARD_STATE_CHANGED: number;
	SCARD_STATE_UNKNOWN: number;
	SCARD_STATE_UNAVAILABLE: number;
	SCARD_STATE_EMPTY: number;
	SCARD_STATE_PRESENT: number;
	SCARD_STATE_ATRMATCH: number;
	SCARD_STATE_EXCLUSIVE: number;
	SCARD_STATE_INUSE: number;
	SCARD_STATE_MUTE: number;
	// Disconnect disposition
	SCARD_LEAVE_CARD: number;
	SCARD_RESET_CARD: number;
	SCARD_UNPOWER_CARD: number;
	SCARD_EJECT_CARD: number;
	name: string;
	state: number;
	connected: boolean;

	on(type: "error", listener: (this: CardReader, error: any) => void): this;

	once(type: "error", listener: (this: CardReader, error: any) => void): this;

	on(type: "end", listener: (this: CardReader) => void): this;

	once(type: "end", listener: (this: CardReader) => void): this;

	on(
		type: "status",
		listener: (this: CardReader, status: Status) => void
	): this;

	once(
		type: "status",
		listener: (this: CardReader, status: Status) => void
	): this;

	SCARD_CTL_CODE(code: number): number;

	connect(options?: ConnectOptions): Promise<number>;

	disconnect(disposition?: number): Promise<void>;

	transmit(
		data: Buffer,
		res_len: number,
		protocol: number
	): Promise<Buffer>;

	control(
		data: Buffer,
		control_code: number,
		res_len: number
	): Promise<Buffer>;

	close(): void;
}

declare function pcsc(): PCSCLite;

export = pcsc;
