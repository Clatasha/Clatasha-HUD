include(FindPackageHandleStandardArgs)

find_path(
  Detours_INCLUDE_DIR
  NAMES detours.h
  DOC "Microsoft Detours include directory"
)

find_library(
  Detours_LIBRARY
  NAMES detours
  DOC "Microsoft Detours library"
)

find_package_handle_standard_args(
  Detours
  REQUIRED_VARS Detours_INCLUDE_DIR Detours_LIBRARY
)

mark_as_advanced(Detours_INCLUDE_DIR Detours_LIBRARY)

if(Detours_FOUND AND NOT TARGET Detours::Detours)
  add_library(Detours::Detours UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    Detours::Detours
    PROPERTIES
      IMPORTED_LOCATION "${Detours_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Detours_INCLUDE_DIR}"
  )
endif()
