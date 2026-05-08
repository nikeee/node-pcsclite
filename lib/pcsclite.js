//@ts-check

const util = require('node:util');
const EventEmitter = require('node:events');

// pcsclite.node is a Node.js native C++ addon that is compiled during installation
// via node-gyp (see package.json > scripts > install)
// the build output name and directory is constant so we can require it directly
// see https://github.com/nodejs/node-gyp/issues/263, https://github.com/nodejs/node-gyp/issues/631
const pcsclite = require('../build/Release/pcsclite.node');

const { PCSCLite: NativePCSCLite, CardReader: NativeCardReader } = pcsclite;

/**
 * @typedef {Object} ConnectOptions
 * @property {number} [share_mode]
 * @property {number} [protocol]
 */

/**
 * @typedef {Object} Status
 * @property {Buffer} [atr]
 * @property {number} state
 */

/** @typedef {any | undefined | null} AnyOrNothing */

util.inherits(NativePCSCLite, EventEmitter);
util.inherits(NativeCardReader, EventEmitter);

/**
 * @param {Buffer} buffer
 * @returns {string[]}
 */
function parseReadersString(buffer) {
	try {
		const string = buffer.toString().slice(0, -1);

		// it looks like
		// ACS ACR122U PICC Interface ACS ACR122U PICC Interface 01
		// [reader_name][reader_name]
		//              ^separator         ^separator^end_separator

		// returns readers in array
		// like [ 'ACS ACR122U PICC Interface', 'ACS ACR122U PICC Interface 01' ]

		return string.split(' ').slice(0, -1);

	} catch (e) {
		return [];
	}
}

/**
 * It returns an array with the elements contained in a that aren't contained in b
 * @param {string[]} a
 * @param {string[]} b
 * @returns {string[]}
 */
function diff(a, b) {
	return a.filter(i => b.indexOf(i) === -1);
}

class PCSCLite extends NativePCSCLite {
	constructor() {
		super();
		/** @type {Object<string, CardReader>} */
		this.readers = {};
		process.nextTick(() => this._poll());
	}

	_poll() {
		// native `start` invokes the callback repeatedly — once per status change —
		// so it cannot be promisified (a Promise would only resolve on the first event).
		this.start((/** @type {AnyOrNothing} */ err, /** @type {Buffer} */ data) => {
			if (err) {
				return this.emit('error', err);
			}

			const names = parseReadersString(data);
			const currentNames = Object.keys(this.readers);
			const newNames = diff(names, currentNames);
			const removedNames = diff(currentNames, names);

			newNames.forEach((name) => {

				const r = new CardReader(name);

				r.on('_end', () => {
					r.removeAllListeners('status');
					delete this.readers[name];
					r.emit('end');
				});

				this.readers[name] = r;

				// native `get_status` is also a multi-fire callback (one call per status change).
				// keep callback form, surface results via 'status'/'error' events.
				r.get_status((/** @type {AnyOrNothing} */ err, /** @type {number} */ state, /** @type {Buffer} */ atr) => {

					if (err) {
						return r.emit('error', err);
					}

					/** @type {Status} */
					const status = { state };

					if (atr) {
						status.atr = atr;
					}

					r.emit('status', status);

					r.state = state;

				});

				this.emit('reader', r);

			});

			removedNames.forEach((name) => {
				this.readers[name].close();
			});
		});
	}
}

class CardReader extends NativeCardReader {
	/**
	 * @param {ConnectOptions} [options]
	 * @returns {Promise<number>}
	 */
	connect(options) {
		options = options || {};
		options.share_mode = options.share_mode || this.SCARD_SHARE_EXCLUSIVE;

		if (typeof options.protocol === 'undefined' || options.protocol === null) {
			options.protocol = this.SCARD_PROTOCOL_T0 | this.SCARD_PROTOCOL_T1;
		}

		if (this.connected) {
			return Promise.resolve(undefined);
		}

		return new Promise((resolve, reject) => {
			this._connect(options.share_mode, options.protocol, (err, protocol) => {
				if (err) {
					return reject(err);
				}
				resolve(protocol);
			});
		});
	}

	/**
	 * @param {number} [disposition]
	 * @returns {Promise<void>}
	 */
	disconnect(disposition) {
		if (typeof disposition !== 'number') {
			disposition = this.SCARD_UNPOWER_CARD;
		}

		if (!this.connected) {
			return Promise.resolve();
		}

		return new Promise((resolve, reject) => {
			this._disconnect(disposition, (err) => {
				if (err) {
					return reject(err);
				}
				resolve();
			});
		});
	}

	/**
	 * @param {Buffer} data
	 * @param {number} res_len
	 * @param {number} protocol
	 * @returns {Promise<Buffer>}
	 */
	transmit(data, res_len, protocol) {
		if (!this.connected) {
			return Promise.reject(new Error('Card Reader not connected'));
		}

		return new Promise((resolve, reject) => {
			this._transmit(data, res_len, protocol, (err, response) => {
				if (err) {
					return reject(err);
				}
				resolve(response);
			});
		});
	}

	/**
	 * @param {Buffer} data
	 * @param {number} control_code
	 * @param {number} res_len
	 * @returns {Promise<Buffer>}
	 */
	control(data, control_code, res_len) {
		if (!this.connected) {
			return Promise.reject(new Error('Card Reader not connected'));
		}

		const output = Buffer.alloc(res_len);

		return new Promise((resolve, reject) => {
			this._control(data, control_code, output, (err, len) => {
				if (err) {
					return reject(err);
				}
				resolve(output.slice(0, len));
			});
		});
	}

	/**
	 * @param {number} code
	 * @returns {number}
	 */
	SCARD_CTL_CODE(code) {
		const isWin = /^win/.test(process.platform);

		if (isWin) {
			return (0x31 << 16 | (code) << 2);
		} else {
			return 0x42000000 + (code);
		}
	}
}

/**
 * @returns {PCSCLite}
 */
module.exports = function () {
	return new PCSCLite();
};
