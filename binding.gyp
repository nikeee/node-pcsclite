{
	"targets": [
		{
			"target_name": "pcsclite",
			"sources": [
				"src/addon.cpp",
				"src/pcsclite.cpp",
				"src/cardreader.cpp"
			],
			"include_dirs": [
				"<!@(node -p \"require('node-addon-api').include\")"
			],
			"defines": [
				"NAPI_DISABLE_CPP_EXCEPTIONS",
				"NAPI_VERSION=8"
			],
			"cflags": [
				"-Wall",
				"-Wextra",
				"-Wno-unused-parameter",
				"-fPIC",
				"-fno-strict-aliasing",
				"-fno-exceptions",
				"-pedantic"
			],
			"cflags_cc": [
				"-fno-exceptions"
			],
			"conditions": [
				[
					"OS=='linux'",
					{
						"include_dirs": [
							"/usr/include/PCSC"
						],
						"link_settings": {
							"libraries": [
								"-lpcsclite"
							],
							"library_dirs": [
								"/usr/lib"
							]
						}
					}
				],
				[
					"OS=='mac'",
					{
						"libraries": [
							"-framework",
							"PCSC"
						]
					}
				],
				[
					"OS=='win'",
					{
						"libraries": [
							"-lWinSCard"
						]
					}
				]
			]
		}
	]
}
