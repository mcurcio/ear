# The node types, each its own library in its own folder (docs/design/batch2.md section 4, docs/design/rtp.md,
# docs/design/sendspin.md). No type links another; the ear binary lists and links all five.
add_subdirectory(white)
add_subdirectory(file)
add_subdirectory(card)
add_subdirectory(rtp)
add_subdirectory(sendspin)
