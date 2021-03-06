# -*- mode: CMAKE; -*-

# swagger
add_custom_target(swagger
  COMMAND ${PROJECT_SOURCE_DIR}/utils/generateSwagger.sh
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# swagger
add_custom_target(examples
  COMMAND ${PROJECT_SOURCE_DIR}/utils/generateExamples.sh
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# manual pages
if (USE_MAINTAINER_MODE AND NOT(MSVC))
  set(MAN_NAMES
    man1/arangobench.1
    man1/arangodump.1
    man1/arangoimp.1
    man1/arangorestore.1
    man1/arangoexport.1
    man1/arangosh.1
    man8/rcarangod.8
    man8/arangod.8
    man8/arango-dfdb.8
    man8/foxx-manager.8
    )

  set(MAN_FILES)
  set(GENERATED_MAN_FILES)


  foreach (m IN LISTS MAN_NAMES)
    set(msrc ${CMAKE_SOURCE_DIR}/Documentation/${m})
    set(mdst ${CMAKE_SOURCE_DIR}/Documentation/man/${m})

    add_custom_command(
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      OUTPUT  ${mdst}
      COMMAND ${CMAKE_SOURCE_DIR}/utils/manPages.sh ${msrc} ${mdst} ${ARANGODB_VERSION}
      DEPENDS ${msrc}
      COMMENT "Building manpage ${mdst}"
      VERBATIM
      )

    list(APPEND MAN_FILES ${mdst})
  endforeach ()

  add_custom_command(
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT  ${CMAKE_SOURCE_DIR}/README
    COMMAND ${CMAKE_SOURCE_DIR}/utils/generateREADME.sh ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/README
    DEPENDS ${CMAKE_SOURCE_DIR}/README.md
    COMMENT "Building README"
    VERBATIM
    )

  list(APPEND MAN_FILES ${CMAKE_SOURCE_DIR}/README)

  add_custom_target(man ALL DEPENDS ${MAN_FILES})

  add_custom_target(clean_man_autogenerated
    COMMAND rm -f ${CMAKE_SOURCE_DIR}/README
    COMMAND rm -f ${MAN_FILES})
  
  list(APPEND CLEAN_AUTOGENERATED_FILES clean_man_autogenerated)
  set(CLEAN_AUTOGENERATED_FILES ${CLEAN_AUTOGENERATED_FILES} PARENT_SCOPE)

endif ()
