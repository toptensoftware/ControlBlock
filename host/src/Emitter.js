// Minimal event emitter (works in Node and browsers)
export class Emitter
{
    #handlers = new Map();

    on(name, fn)
    {
        if (!this.#handlers.has(name))
            this.#handlers.set(name, new Set());
        this.#handlers.get(name).add(fn);
        return this;
    }

    off(name, fn)
    {
        this.#handlers.get(name)?.delete(fn);
        return this;
    }

    emit(name, ...args)
    {
        const set = this.#handlers.get(name);
        if (!set)
            return false;
        for (const fn of [...set])
            fn(...args);
        return set.size > 0;
    }
}
