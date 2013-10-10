IF(WIN32)
	IF(MSVC)
		IF(CMAKE_GENERATOR MATCHES "Win64")
			SET(KLAYGE_ARCH_NAME "x64")
			SET(KLAYGE_VS_PLATFORM_NAME "x64")
		ELSEIF(CMAKE_GENERATOR MATCHES "ARM")
			SET(KLAYGE_ARCH_NAME "arm")
			SET(KLAYGE_VS_PLATFORM_NAME "ARM")
			SET(KLAYGE_WITH_WINRT TRUE)
		ELSE()
			SET(KLAYGE_ARCH_NAME "x86")
			SET(KLAYGE_VS_PLATFORM_NAME "Win32")
		ENDIF()
	ENDIF()
	SET(KLAYGE_PLATFORM_NAME "win")
ENDIF()
IF(UNIX)
	SET(KLAYGE_PLATFORM_NAME "linux")
ENDIF()
IF(KLAYGE_WITH_WINRT)
	SET(KLAYGE_ARCH_NAME ${KLAYGE_ARCH_NAME}_app)
ENDIF()
SET(KLAYGE_PLATFORM_NAME ${KLAYGE_PLATFORM_NAME}_${KLAYGE_ARCH_NAME})
