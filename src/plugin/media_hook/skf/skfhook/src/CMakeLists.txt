SET(LINK_DEPS
	xstudio::media_hook
	xstudio::utility
	xstudio::module
)

create_plugin_with_alias(skfhook xstudio::media_hook::skfhook ${XSTUDIO_GLOBAL_VERSION}  "${LINK_DEPS}")
