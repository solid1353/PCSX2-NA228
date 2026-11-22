// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayDialogs.h"

#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace NetplayDialogs
{
	// How often the room window looks for the room's code.
	static constexpr int ROOM_POLL_MS = 250;
	static constexpr int DEFAULT_INPUT_DELAY = 2;

	std::optional<Netplay::SessionConfig> AskForMatch(QWidget* parent, bool host)
	{
		QDialog dialog(parent);
		dialog.setWindowTitle(host ? QDialog::tr("Host a Netplay Match") : QDialog::tr("Join a Netplay Match"));

		QVBoxLayout* layout = new QVBoxLayout(&dialog);
		QLabel* explanation = new QLabel(host ?
				QDialog::tr("Opens a room for the game selected in the game list. Send the room's code to the other "
							"player; you are player 1.") :
				QDialog::tr("Joins the room whose code the host sent you, on the game selected in the game list; you "
							"are player 2."),
			&dialog);
		explanation->setWordWrap(true);
		layout->addWidget(explanation);
		QLabel* requirement = new QLabel(QDialog::tr("Both players need the same NA228 ISO, BIOS and input delay."), &dialog);
		requirement->setWordWrap(true);
		layout->addWidget(requirement);

		QFormLayout* form = new QFormLayout();
		layout->addLayout(form);
		QLineEdit* code = nullptr;
		if (!host)
		{
			code = new QLineEdit(&dialog);
			code->setMaxLength(6);
			code->setPlaceholderText(QStringLiteral("ABC234"));
			form->addRow(QDialog::tr("Room code:"), code);
		}
		QSpinBox* delay = new QSpinBox(&dialog);
		delay->setRange(1, static_cast<int>(Netplay::MAX_INPUT_DELAY));
		delay->setValue(DEFAULT_INPUT_DELAY);
		delay->setSuffix(QDialog::tr(" frames"));
		form->addRow(QDialog::tr("Input delay:"), delay);

		QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		buttons->button(QDialogButtonBox::Ok)->setText(host ? QDialog::tr("Host") : QDialog::tr("Join"));
		layout->addWidget(buttons);
		QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

		if (code)
		{
			const auto update = [code, buttons]() {
				const QString upper = code->text().trimmed().toUpper();
				if (upper != code->text())
					code->setText(upper);
				buttons->button(QDialogButtonBox::Ok)->setEnabled(Netplay::IsRoomCode(upper.toStdString()));
			};
			QObject::connect(code, &QLineEdit::textEdited, &dialog, update);
			update();
		}

		if (dialog.exec() != QDialog::Accepted)
			return std::nullopt;

		Netplay::SessionConfig config;
		config.host = host;
		if (code)
			config.room_code = code->text().toStdString();
		config.input_delay = static_cast<u32>(delay->value());
		return config;
	}

	void ShowRoom(QWidget* parent)
	{
		QDialog* dialog = new QDialog(parent);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(QDialog::tr("Netplay Room"));

		QVBoxLayout* layout = new QVBoxLayout(dialog);
		QLabel* status = new QLabel(QDialog::tr("Opening a room..."), dialog);
		status->setWordWrap(true);
		layout->addWidget(status);
		QLabel* code = new QLabel(dialog);
		QFont font = code->font();
		font.setPointSizeF(font.pointSizeF() * 2.5);
		font.setBold(true);
		code->setFont(font);
		code->setAlignment(Qt::AlignCenter);
		code->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(code);

		QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
		QPushButton* copy = buttons->addButton(QDialog::tr("Copy Code"), QDialogButtonBox::ActionRole);
		copy->setEnabled(false);
		layout->addWidget(buttons);
		QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
		QObject::connect(copy, &QPushButton::clicked, dialog, [code]() { QGuiApplication::clipboard()->setText(code->text()); });

		QTimer* timer = new QTimer(dialog);
		QObject::connect(timer, &QTimer::timeout, dialog, [timer, status, code, copy]() {
			const Netplay::RoomState room = Netplay::GetRoomState();
			if (!room.end_reason.empty())
			{
				status->setText(QDialog::tr("Netplay ended: %1.").arg(QString::fromStdString(room.end_reason)));
				timer->stop();
			}
			else if (!room.code.empty() && code->text().isEmpty())
			{
				code->setText(QString::fromStdString(room.code));
				status->setText(QDialog::tr("Send this code to the other player. The match starts when they join."));
				copy->setEnabled(true);
			}
		});
		timer->start(ROOM_POLL_MS);
		dialog->show();
	}
} // namespace NetplayDialogs
