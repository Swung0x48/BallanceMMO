# The shared session-client core (docs/session-client-core-design.md 1.1):
# the client session flow, CK-free, under BallanceMMOCommon/src/session.
#
#   BMMO_SESSION_CORE_SRC   its sources, added explicitly to the three targets
#                           that compile it: BallanceMMOSessionClient (the
#                           headless host), BallanceMMOClient (the retail mod)
#                           and BallanceMMOMessageTests (the fake platform).
#
# Not under src/entity or src/utility: those GLOBs also feed the server, the
# mock client and the record parser.  Listed, not globbed, so a new file
# reconfigures every tree that builds it.

get_filename_component(BMMO_SESSION_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/src/session" ABSOLUTE)
set(BMMO_SESSION_CORE_SRC
        "${BMMO_SESSION_CORE_DIR}/client_core.cpp"
        "${BMMO_SESSION_CORE_DIR}/client_core_frame.cpp"
        "${BMMO_SESSION_CORE_DIR}/client_core_held.cpp"
        "${BMMO_SESSION_CORE_DIR}/client_core_lifecycle.cpp"
        "${BMMO_SESSION_CORE_DIR}/client_core_messages.cpp"
        "${BMMO_SESSION_CORE_DIR}/client_journal.cpp")
