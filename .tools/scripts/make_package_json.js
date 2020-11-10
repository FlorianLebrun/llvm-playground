const { script, command, file } = require("./cli")

script((argv) => {
  console.log(`package config of '${argv.source}' to '${argv.destination}'...`)

  const package_json = file.read.json(argv.source) || file.read.json(`${argv.source}/package.json`)
  if(!package_json) throw new Error("Template file for 'package.json' not found")
  package_json.name = argv.name || package_json.name
  package_json.version = argv.version || package_json.version
  if (argv.production) {
    package_json.devDependencies = undefined
    package_json.scripts = undefined
    package_json.private = undefined
  }
  else {
    package_json.private = true
  }
  file.write.json(`${argv.destination}/package.json`, package_json)

  const haveDependencies = package_json.dependencies || package_json.devDependencies || package_json.peerDependencies
  if (argv.install && haveDependencies) {
    console.log(`> package install...`)
    command.exec("npm install", { cwd: argv.destination })
  }
}, {
  arguments: {
    "name": {
      type: "string",
    },
    "version": {
      type: "string",
    },
    "install": {
      type: "boolean"
    },
    "production": {
      type: "boolean",
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
