const { script, command, file } = require("./cli")
const fs = require("fs")

script((argv) => {
  console.log(`build typescript of '${argv.source}' to '${argv.destination}'...`)
  if (!fs.existsSync(`${argv.source}/node_modules`)) {
    command.exec(`npm install`, { cwd: argv.source })
  }
  const args = []
  args.push("--outDir", argv.destination)
  if (argv.tsConfigFile) {
    args.push("--project", argv.tsConfigFile)
  }
  command.exec("npm run build -- " + args.map(x => JSON.stringify(x)).join(" "), {
    cwd: argv.source
  })
}, {
  arguments: {
    "tsConfigFile": {
      type: "string",
    },
    "source": {
      type: "string",
      required: true,
    },
    "destination": {
      type: "string",
      required: true,
    },
  }
})
