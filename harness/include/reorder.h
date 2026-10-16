// Окно переупорядочивания на приёме: придерживает кадры, пришедшие раньше
// срока, и отдаёт их строго по возрастанию seq_id.
//
// Зачем. Потребитель обязан видеть строго возрастающий seq_id, и delivery.h
// отбрасывает всё, что пришло после уже отданного. Пока путь один, порядок
// сохраняется сам. Он ломается там, где путей несколько -- поток одного
// получателя, размазанный по нескольким TX-очередям, или L3-маршрут с ECMP.
// Без окна такое переупорядочивание превращается в потерю: измерено 46%
// выброшенных кадров при разбросе всего на 15 позиций.
//
// Копий нет: окно паркует сам mbuf, а не содержимое. Кадр остаётся лежать там,
// куда его положила карта, и единственная копия на этой стороне -- всё та же
// публикация в кольцо.
//
// Задержка платится только за переупорядочивание. Кадр, пришедший по порядку,
// уходит немедленно: окно на этом пути не стоит ни такта, и однопутевой прогон
// с ним не отличается от прогона без него. Ждёт только кадр после разрыва --
// пока дыра не заполнится или пока окно не переполнится.
//
// Граница ожидания -- размер окна, а не таймер: настоящая потеря дыру не
// закроет никогда, и ждать её вечно нельзя, а таймер означал бы чтение часов на
// горячем пути. Когда окно полно, самый младший кадр отдаётся принудительно и
// разрыв признаётся.
#pragma once

#include <rte_mbuf.h>

#include <cstdint>

namespace reorder {

inline constexpr uint32_t kMaxCapacity = 64;

// Разобранный кадр и mbuf, который его держит. Освобождает mbuf вызывающий:
// окно решает только порядок, временем жизни не распоряжается.
struct Frame {
	rte_mbuf* m = nullptr;
	uint8_t* payload = nullptr;
	uint16_t len = 0;
	uint64_t seq = 0;
};

class Window {
   public:
	struct Stats {
		uint64_t in_order = 0;	// прошло насквозь, ничего не ждало
		uint64_t parked = 0;  // придержано до заполнения дыры
		uint64_t recovered =
			0;	// отдано из окна после того, как дыра закрылась
				// (без фазы наполнения на старте -- см. offer())
		uint64_t forced = 0;  // вытеснено из полного окна, разрыв признан
		uint64_t late = 0;	// пришло позже уже отданного, окно не помогло
		uint32_t peak = 0;	// максимальная занятость
	};

	// capacity == 0 отключает окно: offer() тогда сразу кладёт кадр в готовые,
	// и приёмник ведёт себя ровно как до его появления.
	explicit Window(uint32_t capacity)
		: cap_(capacity > kMaxCapacity ? kMaxCapacity : capacity) {}

	uint32_t capacity() const { return cap_; }
	uint32_t size() const { return n_; }
	const Stats& stats() const { return stats_; }

	// Принять кадр. Готовые к публикации складываются в out (по возрастанию
	// seq_id) -- их может быть от нуля до capacity+1 за один вызов, потому что
	// закрывшаяся дыра выпускает сразу всю накопленную за ней цепочку.
	// out должен вмещать kMaxCapacity + 1.
	uint32_t offer(const Frame& in, Frame* out) {
		if (cap_ == 0) {
			out[0] = in;
			return 1;
		}

		uint32_t k = 0;
		if (!started_) {
			// Фаза наполнения. Начало отсчёта нельзя брать из первого
			// пришедшего кадра: на переупорядоченном пути он сам может быть не
			// первым, и тогда всё, что младше, стало бы "опоздавшим" навсегда.
			// Поэтому сначала копим окно целиком и только потом объявляем
			// минимум началом.
			if (park(in)) return 0;
			started_ = true;
			next_ = slots_[min_index()].seq;
			k = release(out);
			// Фаза наполнения к переупорядочиванию отношения не имеет: окно
			// забивается целиком ВСЕГДА, на любом, даже идеально упорядоченном
			// потоке. Пока эти cap кадров считались в recovered и peak,
			// статистика показывала работу окна там, где её не было: на одной
			// очереди выходило recovered = cap и peak = cap из cap, то есть
			// "окно спасло cap кадров и было забито под завязку" -- при нулевом
			// переупорядочивании. Обнуляем.
			stats_.recovered = 0;
			stats_.peak = 0;
			// in не пристроен -- он проходит обычным путём ниже.
		}
		return k + accept(in, out + k);
	}

	// Обычный путь: кадр уже после фазы наполнения.
	uint32_t accept(const Frame& in, Frame* out) {
		if (in.seq < next_) {  // старее уже отданного -- дыра пройдена
			++stats_.late;
			out[0] = in;
			return 1;
		}

		uint32_t k = 0;
		if (in.seq == next_) {
			out[k++] = in;
			++next_;
			++stats_.in_order;
			k +=
				release(out + k);  // дыра закрылась -- выпустить цепочку за ней
			return k;
		}

		if (park(in)) {	 // разрыв: придержать, пока есть место
			++stats_.parked;
			return 0;
		}

		// Окно полно: младший уходит принудительно, разрыв признан.
		const uint32_t i = min_index();
		out[k++] = slots_[i];
		++stats_.forced;
		next_ = slots_[i].seq + 1;
		slots_[i] = in;
		k += release(out + k);
		return k;
	}

	// Слить окно при остановке, чтобы придержанные кадры не пропали молча.
	uint32_t drain(Frame* out) {
		uint32_t k = 0;
		while (n_ > 0) {
			const uint32_t i = min_index();
			out[k++] = slots_[i];
			slots_[i] = slots_[--n_];
		}
		return k;
	}

   private:
	// Выпустить непрерывную цепочку, начиная с next_.
	uint32_t release(Frame* out) {
		uint32_t k = 0;
		for (;;) {
			bool found = false;
			for (uint32_t i = 0; i < n_; ++i) {
				if (slots_[i].seq == next_) {
					out[k++] = slots_[i];
					slots_[i] = slots_[--n_];
					++next_;
					++stats_.recovered;
					found = true;
					break;
				}
			}
			if (!found) return k;
		}
	}

	bool park(const Frame& f) {
		if (n_ >= cap_) return false;
		slots_[n_++] = f;
		if (n_ > stats_.peak) stats_.peak = n_;
		return true;
	}

	uint32_t min_index() const {
		uint32_t best = 0;
		for (uint32_t i = 1; i < n_; ++i) {
			if (slots_[i].seq < slots_[best].seq) best = i;
		}
		return best;
	}

	Frame slots_[kMaxCapacity];
	uint32_t cap_;
	uint32_t n_ = 0;
	uint64_t next_ = 0;
	bool started_ = false;
	Stats stats_;
};

}  // namespace reorder
