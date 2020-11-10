const { script, command, file } = require("./cli")
const Path = require("path")

script((argv) => {
  if (file.exists(argv.input)) {
    console.log(`copy of '${argv.input}' to '${argv.output || argv.destination}'...`)
    if (argv.output) file.copy.toFile(argv.input, argv.output)
    else if (argv.destination) file.copy.toDir(argv.input, argv.destination)
    else command.exit(1)
  }
  else {
    console.log(`copy empty of '${argv.input}'`)
    if (argv.output) file.remove(argv.output)
    else if (argv.destination) file.remove(Path.join(argv.destination, Path.basename(argv.input)))
    else command.exit(1)
  }
}, {
  arguments: {
    "input": {
      type: "string",
      required: true,
    },
    "output": {
      type: "string",
    },
    "destination": {
      type: "string",
    },
  }
})
