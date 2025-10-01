#pragma once
#ifndef MYTIMER_H
#define MYTIMER_H
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <chrono>
#include <llvm/Support/Debug.h>
#include <memory>

class Timer {
public:
	Timer() { m_StartTimepoint = std::chrono::high_resolution_clock::now(); }
	Timer(std::string func_name)
	{
		m_Name = func_name;
		m_StartTimepoint = std::chrono::high_resolution_clock::now();
	}

	std::pair<double, double> Stop()
	{
		auto endTimepoint = std::chrono::high_resolution_clock::now();
		auto start =
			std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimepoint)
				.time_since_epoch()
				.count();
		auto end = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint)
				   .time_since_epoch()
				   .count();

		auto us = end - start;
		double ms = us * 0.001;
		llvm::dbgs() << "Time";
		if (m_Name.size())
			llvm::dbgs() << "<" << m_Name << ">: ";

		llvm::dbgs() << ms << "ms\n";
		return {us, ms};
	}
	~Timer() { Stop(); }

private:
	std::chrono::time_point<std::chrono::high_resolution_clock> m_StartTimepoint;
	std::string m_Name{};
};

#endif