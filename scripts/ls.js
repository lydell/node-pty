const fs = require('fs');
const path = require('path');

const BUILD = path.join(__dirname, '..', 'build');

function tree(dir) {
    let result = '';
    const files = fs.readdirSync(dir);
    files.forEach(file => {
        const curPath = path.join(dir, file);
        const stats = fs.lstatSync(curPath);
        if (stats.isDirectory()) {
            result += `${curPath}\n${tree(curPath)}`;
        } else {
            result += `${curPath}\n`;
        }
    });
    return result;
}

console.log(tree(BUILD));
