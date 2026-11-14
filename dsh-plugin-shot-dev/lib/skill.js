/**
 * Plugin-shot 插件开发技能：把仓库里的 skills/plugin-shot-dev 注册给 DSH 技能目录。
 *
 * 只做一件事：读 SKILL.md 的 frontmatter（name/description）构造一条
 * `bundled` 来源、rank 600 的技能目录项，再通过 `ctx.skills.registerProvider()`
 * 注册；正文在每次加载时重读，改 SKILL.md 不需要重启。
 *
 * 不引入任何 DSH 包依赖：只用 Node 内置模块，接口形状对齐
 * `@deepseek-ai/dsh-skill` 的 SkillProvider（list/get），与宿主内置技能同一路径。
 *
 * @module dsh-plugin-shot-dev/skill
 */
import { readFileSync } from 'node:fs'
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'

/** 技能行名（bundle 内唯一，与 cordis.patch.yml 的 id 对应）。 */
export const name = 'dsh-plugin-shot-dev-skill'

/** 行依赖：技能注册表由宿主注入，没有它就装不上技能。 */
export const inject = ['skills']

/** 技能名：与 SKILL.md frontmatter 的 name 必须完全一致。 */
const SKILL_NAME = 'plugin-shot-dev'

/** SKILL.md 路径（本模块位于包内 lib/ 下）。 */
const SKILL_FILE = fileURLToPath(new URL('../skills/plugin-shot-dev/SKILL.md', import.meta.url))

/** 技能资源目录（SKILL.md 里引用的 references/ 相对它解析）。 */
const SKILL_DIRECTORY = fileURLToPath(new URL('../skills/plugin-shot-dev/', import.meta.url))

/** 技能提供者名：与包名一致，便于在技能列表里定位。 */
const SKILL_PROVIDER = 'dsh-plugin-shot-dev'

/** 与宿主内置 bundle 技能同级的排序权重；用户自己同名技能仍然优先。 */
const BUNDLED_SKILL_RANK = 600

/** 模型与用户都可主动调用，和宿主内置技能一致。 */
const INVOCATION = { modelInvocable: true, userInvocable: true }

/**
 * 解析 SKILL.md 的 frontmatter 与正文。
 *
 * 只读单行的 `name:` / `description:`；接受 BOM 与 CRLF（Windows 检出常见），
 * 缺少任一项视为无效技能而不是抛异常。
 * @param {string} raw 文件原文。
 * @returns {{name: string, description: string, content: string} | undefined}
 */
export function parseSkill(raw) {
    const match = /^\uFEFF?---\r?\n([\s\S]*?)\r?\n---[ \t]*(?:\r?\n|$)/u.exec(raw)
    if (match === null) return undefined
    const fields = new Map()
    for (const line of match[1].split(/\r?\n/u)) {
        const field = /^([A-Za-z-]+):[ \t]*(.*?)[ \t]*$/u.exec(line)
        if (field !== null) fields.set(field[1], unquote(field[2]))
    }
    const skillName = fields.get('name')
    const description = fields.get('description')
    if (skillName === undefined || skillName === '' || description === undefined || description === '') {
        return undefined
    }
    return { name: skillName, description, content: raw.slice(match[0].length).replace(/\r\n/gu, '\n').trim() }
}

/**
 * 去掉一对匹配的 YAML 引号。
 * @param {string} value 原始字段值。
 * @returns {string} 去引号后的值。
 */
function unquote(value) {
    return /^(["']).*\1$/u.test(value) ? value.slice(1, -1) : value
}

/**
 * 读技能目录项。
 * @returns {object | undefined} 供 registerProvider 使用的候选条目；文件缺失/不合法返回 undefined。
 */
function readCandidate() {
    let raw
    try {
        raw = readFileSync(SKILL_FILE, 'utf8')
    } catch {
        console.warn(`dsh-plugin-shot-dev: 无法读取技能文件 ${SKILL_FILE}`)
        return undefined
    }
    const parsed = parseSkill(raw)
    if (parsed === undefined) {
        console.warn(`dsh-plugin-shot-dev: ${SKILL_FILE} 缺少 name/description frontmatter`)
        return undefined
    }
    if (parsed.name !== SKILL_NAME) {
        console.warn(`dsh-plugin-shot-dev: 技能名是 "${parsed.name}"，应为 "${SKILL_NAME}"`)
        return undefined
    }
    return {
        name: parsed.name,
        description: parsed.description,
        invocation: { ...INVOCATION },
        provider: SKILL_PROVIDER,
        source: 'bundled',
        path: SKILL_FILE,
        resourceBase: { kind: 'directory', path: SKILL_DIRECTORY },
        rank: BUNDLED_SKILL_RANK,
        locator: SKILL_FILE,
    }
}

/**
 * 由候选条目构造技能提供者。
 * @param {object} candidate list() 返回的目录项。
 * @returns {object} 符合宿主 SkillProvider 契约的提供者。
 */
function createSkillProvider(candidate) {
    return {
        name: SKILL_PROVIDER,
        list: () => Promise.resolve([{ ...candidate, invocation: { ...candidate.invocation }, resourceBase: { ...candidate.resourceBase } }]),
        async get(listed) {
            let raw
            try {
                raw = await readFile(listed.locator, { encoding: 'utf8' })
            } catch {
                return undefined
            }
            const parsed = parseSkill(raw)
            if (parsed === undefined || parsed.name !== listed.name) return undefined
            const { rank: _rank, locator: _locator, ...summary } = listed
            return { ...summary, description: parsed.description, content: parsed.content }
        },
    }
}

/**
 * 技能行入口：注册技能提供者。
 * @param {object} ctx 行上下文；`skills` 由 inject 保证存在。
 */
export function apply(ctx) {
    const candidate = readCandidate()
    if (candidate === undefined) return
    ctx.skills.registerProvider(() => createSkillProvider(candidate))
}
