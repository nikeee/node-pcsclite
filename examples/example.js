"use strict";

const pcsclite = require('../lib/pcsclite');


const pcsc = pcsclite();

pcsc.on('reader', (reader) => {

	console.log('New reader detected', reader.name);

	reader.on('error', err => {
		console.log('Error(', reader.name, '):', err.message);
	});

	reader.on('status', async (status) => {

		console.log('Status(', reader.name, '):', status);

		// check what has changed
		const changes = reader.state ^ status.state;

		if (!changes) {
			return;
		}

		if ((changes & reader.SCARD_STATE_EMPTY) && (status.state & reader.SCARD_STATE_EMPTY)) {

			console.log("card removed");

			try {
				await reader.disconnect(reader.SCARD_LEAVE_CARD);
				console.log('Disconnected');
			} catch (err) {
				console.log(err);
			}

		}
		else if ((changes & reader.SCARD_STATE_PRESENT) && (status.state & reader.SCARD_STATE_PRESENT)) {

			console.log("card inserted");

			try {
				const protocol = await reader.connect({ share_mode: reader.SCARD_SHARE_SHARED });
				console.log('Protocol(', reader.name, '):', protocol);

				const data = await reader.transmit(Buffer.from([0x00, 0xB0, 0x00, 0x00, 0x20]), 40, protocol);
				console.log('Data received', data);
				reader.close();
				pcsc.close();
			} catch (err) {
				console.log(err);
			}

		}

	});

	reader.on('end', () => {
		console.log('Reader', reader.name, 'removed');
	});

});

pcsc.on('error', err => {
	console.log('PCSC error', err.message);
});
