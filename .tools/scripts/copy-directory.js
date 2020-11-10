const { script, command, directory } = require("./cli")
const Path = require("path")

script((argv) => {
  let filter
  if (argv.filter) {
    const filterKeys = argv.filter.toLowerCase().split("|")
    filter = (name) => {
      const lname = name.toLowerCase()
      if (filterKeys.indexOf(lname) >= 0) return true
      if (filterKeys.indexOf("*" + Path.extname(lname)) >= 0) return true
      return false
    }
  }
  for (const source of argv.sources) {
    if (directory.exists(source)) {
      console.log(`copy of directory '${source}' to '${argv.destination}'...`)
      directory.copy(source, argv.destination, filter)
    }
    else {
      console.log(`directory '${source}' not found`)
      command.exit(1)
    }
  }
}, {
  arguments: {
    "filter": {
      type: "string",
    },
    "sources": {
      type: "string",
      isArray: true,
      required: true,
    },
    "destination": {
      type: "string",
      required: true,
    },
  }
})
