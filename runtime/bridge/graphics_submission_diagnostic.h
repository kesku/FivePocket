#pragma once
#include <array>
#include <exception>
#include <ostream>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

// Observes the existing short-circuit dispatch. No callback is retried, no false
// becomes true, and the original exception is rethrown to the execution guard.
// The caller serializes after all guest workers have joined, not during a draw.
class GraphicsSubmissionDiagnostic {
public:
    enum class Outcome { NotApplicable, False, True, StandardException, OtherException };
    struct Step { std::array<char,48> name{}; Outcome outcome=Outcome::NotApplicable; };
    void begin(unsigned word) noexcept { *this={}; writeWord_=word; started_=true; }
    void finish(bool accepted) noexcept { returned_=true; accepted_=accepted; }
    template<class F> auto attempt(std::string_view name,F&& operation) -> decltype(operation()) {
        Step* step=count_<steps_.size()?&steps_[count_++]:nullptr;
        if(step)copy(step->name,name);else truncated_=true;
        try {
            auto result=std::forward<F>(operation)();
            if(step) {
                if constexpr(std::is_same_v<decltype(result),std::optional<bool>>)
                    step->outcome=!result?Outcome::NotApplicable:(*result?Outcome::True:Outcome::False);
                else step->outcome=result?Outcome::True:Outcome::False;
            }
            return result;
        } catch(const std::exception& e) {
            if(step)step->outcome=Outcome::StandardException;
            // Do not scan/allocate an unbounded exception message in the failure path.
            const char* message=e.what();
            if(message)for(std::size_t i=0;i+1<exception_.size() && message[i];++i)
                exception_[i]=message[i];
            throw;
        } catch(...) { if(step)step->outcome=Outcome::OtherException;throw; }
    }
    void json(std::ostream& out) const {
        out<<"{\"started\":"<<(started_?"true":"false")<<",\"write_word\":"<<writeWord_
           <<",\"returned\":"<<(returned_?"true":"false")<<",\"accepted\":"<<(accepted_?"true":"false")
           <<",\"truncated\":"<<(truncated_?"true":"false")<<",\"exception\":";
        quote(out,exception_.data());out<<",\"steps\":[";
        constexpr const char* names[]{"not-applicable","returned-false","returned-true","std-exception","other-exception"};
        for(std::size_t i=0;i<count_;++i) {
            if(i)out<<',';out<<"{\"handler\":";quote(out,steps_[i].name.data());
            out<<",\"outcome\":\""<<names[static_cast<unsigned>(steps_[i].outcome)]<<"\"}";
        }
        out<<"]}";
    }
private:
    template<std::size_t N> static void copy(std::array<char,N>& out,std::string_view value) noexcept {
        for(std::size_t i=0;i+1<N && i<value.size();++i)out[i]=value[i];
    }
    static void quote(std::ostream& out,const char* text) {
        out<<'"';for(;*text;++text) {
            const auto c=static_cast<unsigned char>(*text);
            if(c=='"' || c=='\\')out<<'\\';
            out<<static_cast<char>(c<32 || c>126?' ':c);
        }out<<'"';
    }
    std::array<Step,32> steps_{};
    std::array<char,160> exception_{};
    std::size_t count_=0;
    unsigned writeWord_=0;
    bool started_=false,returned_=false,accepted_=false,truncated_=false;
};
